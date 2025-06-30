from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable, Optional, TYPE_CHECKING
from typing_extensions import final, override

import torch._inductor.async_compile  # noqa: F401 required to warm up AsyncCompile pools
from torch._inductor.output_code import CompiledFxGraphConstants, OutputCode

from .compile_fx import _CompileFxKwargs, _InProcessFxCompile, FxCompile
from .output_code import complex_memory_overlap as complex_memory_overlap  # noqa: F401


if TYPE_CHECKING:
    from collections.abc import Sequence
    from concurrent.futures import Future

    from torch._inductor.utils import InputType
    from torch.fx import GraphModule

    from .compile_fx_ext import _OutOfProcessFxCompile, _WireProtocolPickledOutput


@dataclass
class _PostCompileData:
    example_inputs: Sequence[InputType]
    constants: CompiledFxGraphConstants
    graph_kwargs: _CompileFxKwargs


# _AsyncOutputCode handles the actual management of waiting for an
# out-of-process compile to finish and then switching over to it.
@final
class _AsyncOutputCode(OutputCode):
    _eager_forward: Optional[Callable[..., Any]]
    _output_code: Optional[OutputCode]
    _future: Optional[Future[_WireProtocolPickledOutput]]
    _callback: Callable[[_WireProtocolPickledOutput], OutputCode]
    _post_compile_data: Optional[_PostCompileData] = None
    _boxed_call: bool  # Copied from the forward/output_code

    def __init__(
        self,
        # eager_forward is run until the future is finished.
        eager_forward: Callable[..., Any],
        # this responds with the result of the out-of-process compile when it's
        # ready.
        future: Future[_WireProtocolPickledOutput],
        # this callback gets called to turn the _WireProtocolPickledOutput into an OutputCode
        callback: Callable[[_WireProtocolPickledOutput], OutputCode],
    ) -> None:
        self._eager_forward = eager_forward
        self._boxed_call = getattr(eager_forward, "_boxed_call", False)
        self._output_code = None

        self._future = future
        self._callback = callback

    @override
    def __call__(self, *args: Any) -> Any:
        if self._future is not None and self._future.done():
            args = self._switch_to_compiled_forward(args)

        if eager_forward := self._eager_forward:
            _AsyncFxCompile._stat_eager_runs += 1
            return eager_forward(*args)

        else:
            _AsyncFxCompile._stat_compiled_runs += 1
            assert self._output_code is not None
            return self._output_code.__call__(*args)

    # Takes and returns the args (converted to the "right" boxed mode)
    def _switch_to_compiled_forward(self, args: tuple[Any, ...]) -> tuple[Any, ...]:
        assert self._future is not None

        # TODO: If the future ended in an exception do we want to continue
        # running eager or hit the exception now?
        f, self._future = self._future, None
        output_code = self._callback(f.result())

        if pcd := self._post_compile_data:
            self._post_compile_data = None

            output_code.post_compile(
                pcd.example_inputs, pcd.constants, pcd.graph_kwargs
            )

        self._output_code = output_code
        self._eager_forward = None
        boxed_call = getattr(output_code, "_boxed_call", False)

        if self._boxed_call != boxed_call:
            if self._boxed_call:
                # Was boxed, now unboxed
                args = args[0] if len(args) > 0 else ()
            else:
                # Was unboxed, now boxed
                args = (args,)

        self._boxed_call = boxed_call
        return args

    @override
    def post_compile(
        self,
        example_inputs: Sequence[InputType],
        constants: CompiledFxGraphConstants,
        graph_kwargs: _CompileFxKwargs,
    ) -> None:
        if self._eager_forward is not None:
            self._post_compile_data = _PostCompileData(
                example_inputs, constants, graph_kwargs
            )
        else:
            assert self._output_code is not None
            self._output_code.post_compile(example_inputs, constants, graph_kwargs)


# Given an FxCompile for an out-of-process compile _AsyncFxCompile will run
# eager until the compiled artifact is ready then it will automatically switch
# over to using the compiled version.
@final
class _AsyncFxCompile(FxCompile):
    _compile: _OutOfProcessFxCompile

    # Some debugging stats:
    # Number of times we started a background compile.
    _stat_bg_started: int = 0
    # Number of times we finished a background compile.
    _stat_bg_finished: int = 0
    # Number of times we ran "eager"
    _stat_eager_runs: int = 0
    # Number of times we ran our compiled (out-of-process) artifact
    _stat_compiled_runs: int = 0

    def __init__(self, compile: _OutOfProcessFxCompile) -> None:
        self._compile = compile

    @classmethod
    def _reset_stats(cls) -> None:
        cls._stat_bg_started = 0
        cls._stat_bg_finished = 0
        cls._stat_eager_runs = 0
        cls._stat_compiled_runs = 0

    @override
    def codegen_and_compile(
        self,
        gm: GraphModule,
        example_inputs: Sequence[InputType],
        inputs_to_check: Sequence[int],
        graph_kwargs: _CompileFxKwargs,
    ) -> OutputCode:
        eager_output_code = _InProcessFxCompile().codegen_and_compile(
            gm, example_inputs, inputs_to_check, graph_kwargs
        )

        # This is similar to _SerializedFxCompile.codegen_and_compile() but
        # handles the async routing.

        serialized = self._compile.serialize_compile(
            gm, example_inputs, inputs_to_check, graph_kwargs
        )
        if not serialized:
            # We can't serialize - just return the eager OutputCode
            return eager_output_code

        inputs, constants = serialized

        _AsyncFxCompile._stat_bg_started += 1
        f = self._compile._send_to_child_async(inputs)

        # This is called by _switch_to_compiled_forward() when f has a result...
        def callback(pickled_output: _WireProtocolPickledOutput) -> OutputCode:
            _AsyncFxCompile._stat_bg_finished += 1
            output = pickled_output.deserialize(constants)
            self._compile._postprocess(output)
            return output.graph

        return _AsyncOutputCode(eager_output_code, f, callback)


# _ProgressiveOutputCode handles running a fast compile first, then hot-swapping
# to a more optimized version when the expensive compile finishes.
@final
class _ProgressiveOutputCode(OutputCode):
    _fast_output_code: Optional[OutputCode]
    _optimized_output_code: Optional[OutputCode]
    _progression_futures: list[Future[_WireProtocolPickledOutput]]
    _callback: Callable[[_WireProtocolPickledOutput], OutputCode]
    _post_compile_data: Optional[_PostCompileData] = None
    _boxed_call: bool
    _current_progression_index: int

    def __init__(
        self,
        # Fast compile that runs immediately
        fast_output_code: OutputCode,
        # Futures for the progressive optimized compiles
        progression_futures: list[Future[_WireProtocolPickledOutput]],
        # Callback to convert the optimized result to OutputCode
        callback: Callable[[_WireProtocolPickledOutput], OutputCode],
    ) -> None:
        self._fast_output_code = fast_output_code
        self._optimized_output_code = None
        self._progression_futures = progression_futures
        self._callback = callback
        self._boxed_call = getattr(fast_output_code, "_boxed_call", False)
        self._current_progression_index = -1

    @override
    def __call__(self, *args: Any) -> Any:
        # Check if any newer progression stage is ready and switch to it
        args = self._check_and_switch_progression(args)

        if self._optimized_output_code is not None:
            _ProgressiveFxCompile._stat_optimized_runs += 1
            return self._optimized_output_code.__call__(*args)
        else:
            _ProgressiveFxCompile._stat_fast_runs += 1
            assert self._fast_output_code is not None
            return self._fast_output_code.__call__(*args)

    def _check_and_switch_progression(self, args: tuple[Any, ...]) -> tuple[Any, ...]:
        # Check if any newer progression stage is ready (in order from latest to earliest)
        for i in range(
            len(self._progression_futures) - 1, self._current_progression_index, -1
        ):
            future = self._progression_futures[i]
            if future.done():
                args = self._switch_to_progression_stage(i, args)
                break

        return args

    def _switch_to_progression_stage(
        self, stage_index: int, args: tuple[Any, ...]
    ) -> tuple[Any, ...]:
        future = self._progression_futures[stage_index]
        optimized_output_code = self._callback(future.result())

        if pcd := self._post_compile_data:
            self._post_compile_data = None
            optimized_output_code.post_compile(
                pcd.example_inputs, pcd.constants, pcd.graph_kwargs
            )

        self._optimized_output_code = optimized_output_code
        self._fast_output_code = None
        self._current_progression_index = stage_index

        optimized_boxed_call = getattr(optimized_output_code, "_boxed_call", False)

        if self._boxed_call != optimized_boxed_call:
            if self._boxed_call:
                # Was boxed, now unboxed
                args = args[0] if len(args) > 0 else ()
            else:
                # Was unboxed, now boxed
                args = (args,)

        self._boxed_call = optimized_boxed_call
        return args

    @override
    def post_compile(
        self,
        example_inputs: Sequence[InputType],
        constants: CompiledFxGraphConstants,
        graph_kwargs: _CompileFxKwargs,
    ) -> None:
        if self._optimized_output_code is not None:
            self._optimized_output_code.post_compile(
                example_inputs, constants, graph_kwargs
            )
        elif self._fast_output_code is not None:
            self._fast_output_code.post_compile(example_inputs, constants, graph_kwargs)
            # Store for later when  optimized version is ready
            self._post_compile_data = _PostCompileData(
                example_inputs, constants, graph_kwargs
            )


# _ProgressiveFxCompile runs a fast compile immediately, then kicks off
# progressive compiles in the background and hot-swaps when they're ready.
@final
class _ProgressiveFxCompile(FxCompile):
    _fast_compile: FxCompile
    _optimized_compile: _OutOfProcessFxCompile
    _progression_configs: list[dict[str, Any]]

    # Debugging stats
    _stat_bg_started: int = 0
    _stat_bg_finished: int = 0
    _stat_fast_runs: int = 0
    _stat_optimized_runs: int = 0

    def __init__(
        self,
        fast_compile: FxCompile,
        optimized_compile: _OutOfProcessFxCompile,
        progression_configs: list[dict[str, Any]],
    ) -> None:
        self._fast_compile = fast_compile
        self._optimized_compile = optimized_compile
        self._progression_configs = progression_configs

    @classmethod
    def _reset_stats(cls) -> None:
        cls._stat_bg_started = 0
        cls._stat_bg_finished = 0
        cls._stat_fast_runs = 0
        cls._stat_optimized_runs = 0

    @override
    def codegen_and_compile(
        self,
        gm: GraphModule,
        example_inputs: Sequence[InputType],
        inputs_to_check: Sequence[int],
        graph_kwargs: _CompileFxKwargs,
    ) -> OutputCode:
        # First run the fast compile
        fast_output_code = self._fast_compile.codegen_and_compile(
            gm, example_inputs, inputs_to_check, graph_kwargs
        )

        # Start the progressive compiles in the background
        serialized = self._optimized_compile.serialize_compile(
            gm, example_inputs, inputs_to_check, graph_kwargs
        )

        if not serialized:
            # Can't serialize - just return the fast version
            return fast_output_code

        inputs, constants = serialized

        import torch._inductor.config as inductor_config

        progression_futures: list[Future[_WireProtocolPickledOutput]] = []

        for config in self._progression_configs:
            with inductor_config.patch(config):
                _ProgressiveFxCompile._stat_bg_started += 1
                future = self._optimized_compile._send_to_child_async(inputs)
                progression_futures.append(future)

        # Callback to handle the optimized result
        def callback(pickled_output: _WireProtocolPickledOutput) -> OutputCode:
            _ProgressiveFxCompile._stat_bg_finished += 1
            output = pickled_output.deserialize(constants)
            self._optimized_compile._postprocess(output)
            return output.graph

        return _ProgressiveOutputCode(fast_output_code, progression_futures, callback)
