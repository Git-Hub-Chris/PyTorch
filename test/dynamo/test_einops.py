# Owner(s): ["module: dynamo"]
import importlib
import os
import subprocess
import sys
import unittest

import torch
import torch._dynamo.config
import torch._dynamo.test_case
from torch import nn
from torch._dynamo.exc import Unsupported
from torch._dynamo.test_case import TestCase
from torch.testing._internal.common_utils import (
    instantiate_parametrized_tests,
    parametrize,
)


HAS_EINOPS = importlib.util.find_spec("einops")

if HAS_EINOPS:
    import einops

    einops_version = einops.__version__
else:
    einops_version = "none"
einops_version_sanitized = einops_version.replace(".", "_")


@unittest.skipIf(not HAS_EINOPS, "these tests require einops")
class TestEinops(TestCase):
    """
    These tests adapted from similar tests in the einops repo.
    https://github.com/arogozhnikov/einops/blob/main/einops/tests/test_other.py#L254

    The goal of this test suite is to test torch.compile x einops for multiple
    versions of einops. Our goal is to prevent regressions in einops from changes
    in PyTorch.
    """

    @unittest.skipIf(
        einops_version == "0.6.1", "https://github.com/pytorch/pytorch/issues/157417"
    )
    @parametrize("version", [einops_version_sanitized])
    def test_functions(self, version):
        from einops import einsum, pack, rearrange, reduce, repeat, unpack

        class TorchModuleWithOperations(nn.Module):
            def __init__(self) -> None:
                super().__init__()

            def forward(self, x_abc, suffix=""):
                a, b, c = x_abc.shape

                def suf(pattern):
                    parts = pattern.split()
                    return " ".join(
                        [p if p[-1] not in "acd" else p + suffix for p in parts]
                    )

                # patterns look a bit strange because names a, c, d will be modified on every run
                # by suf function
                x_abcd = repeat(x_abc, suf("a b c -> a b c 4"))
                x_abc = reduce(x_abcd, suf("a b c d -> a b c"), "min")
                x_abdc, ps = pack([x_abc] * (2 + len(suffix)), suf("a b * c"))
                x_array = unpack(
                    rearrange(x_abdc, suf("a b d c -> (a b ) 1 c d")), ps, "ab one1 c *"
                )
                x1 = x_array[0] + len(x_array)
                x1 = rearrange(x1, suf("(a b ) 1 c -> a b c"), b=b)
                addition = einsum(x_abc, x_abcd, suf("a b c , a b c d -> d"))[0]
                return x1 + addition

        original = TorchModuleWithOperations()
        # Einops only interacts with Dynamo but we test backend="inductor" just in case
        compiled = torch.compile(original, backend="inductor", fullgraph=True)
        for size in [10, 20, 40]:
            x = torch.rand([size, size + 1, size + 2])
            for suffix in ["", "suf1", "other_suffix"]:
                result1 = compiled(x, suffix)
                result2 = original(x.double(), suffix).float()
                self.assertEqual(result1, result2)

    @parametrize("version", [einops_version_sanitized])
    def test_layers(self, version):
        from einops.layers.torch import EinMix, Rearrange, Reduce

        original = nn.Sequential(
            Rearrange("b (t c) -> b t c", c=16),
            EinMix(
                "b t c -> qkv b t cout",
                weight_shape="qkv c cout",
                bias_shape="qkv cout",
                qkv=3,
                c=16,
                cout=8,
            ),
            Reduce("qkv b t cout -> b t qkv", "min", cout=8),
        )

        # Einops only interacts with Dynamo but we test backend="inductor" just in case
        compiled = torch.compile(original, backend="inductor", fullgraph=True)

        for size in [16, 32, 64]:
            x = torch.rand([size, size])
            result1 = original(x)
            result2 = compiled(x.double()).float()
            self.assertEqual(result1, result2)

    @parametrize("version", [einops_version_sanitized])
    def test_no_recompile_on_lazy_state(self, version):
        """einops has some lazy state that gets initialized the first time an API
        is called. This should not trigger a recompile."""
        script = """\
import torch
import torch.nn as nn
from einops import einsum, pack, reduce, repeat, unpack, rearrange

class TorchModuleWithOperations(nn.Module):
    def __init__(self) -> None:
        super().__init__()

    def forward(self, x_abc, suffix=""):
        a, b, c = x_abc.shape

        def suf(pattern):
            parts = pattern.split()
            return " ".join([p if p[-1] not in "acd" else p + suffix for p in parts])

        # patterns look a bit strange because names a, c, d will be modified on every run
        # by suf function
        x_abcd = repeat(x_abc, suf("a b c -> a b c 4"))
        x_abc = reduce(x_abcd, suf("a b c d -> a b c"), "min")
        x_abdc, ps = pack([x_abc] * (2 + len(suffix)), suf("a b * c"))
        x_array = unpack(rearrange(x_abdc, suf("a b d c -> (a b ) 1 c d")), ps, "ab one1 c *")
        x1 = x_array[0] + len(x_array)
        x1 = rearrange(x1, suf("(a b ) 1 c -> a b c"), b=b)
        addition = einsum(x_abc, x_abcd, suf("a b c , a b c d -> d"))[0]
        return x1 + addition

compiled_fn = torch.compile(TorchModuleWithOperations(), fullgraph=True)
x = torch.arange(2 * 3 * 5).view(2, 3, 5)
y = compiled_fn(x)

# Should not recompile!
with torch.compiler.set_stance("fail_on_recompile"):
    z = compiled_fn(x)
"""
        subprocess.check_output([sys.executable, "-c", script])

    def _run_in_subprocess(self, flag, method, einops_method, snippet):
        # run in a different process
        script = f"""
import torch
from torch._dynamo.testing import EagerAndRecordGraphs, normalize_gm

torch._dynamo.config.enable_einops_tracing={flag}

import einops

backend = EagerAndRecordGraphs()

def f(x):
    {snippet}
    return y.sin()

x = torch.randn(3, 4, 5)
expected = f(x)
got = torch.compile(f, backend=backend, fullgraph=True)(x)

assert torch.allclose(expected, got)
assert len(backend.graphs) == 1, len(backend.graphs)
graph = backend.graphs[0]
print(normalize_gm(graph.print_readable(print_output=False)))
"""
        script = script.strip()
        try:
            output = (
                subprocess.check_output(
                    [sys.executable, "-c", script],
                    stderr=subprocess.STDOUT,
                    cwd=os.path.dirname(os.path.realpath(__file__)),
                )
                .decode("utf-8")
                .strip()
            )
        except subprocess.CalledProcessError as e:
            self.fail(
                msg=(f"Subprocess exception {method}:\n" + e.output.decode("utf-8"))
            )
        else:
            if flag:
                self.assertNotIn(einops_method, output)
            else:
                self.assertIn(einops_method, output)

    @parametrize(
        "method",
        ["reduce", "repeat", "pack", "unpack", "einsum", "rearrange"],
        name_fn=lambda f: f,
    )
    @parametrize("flag", [True, False], name_fn=lambda f: f)
    def test_einops_method(self, flag, method):
        if not hasattr(einops, method):
            self.skipTest(f"Needs einops.{method}")

        if method == "reduce":
            einops_method = f"einops_einops_{method}"
            snippet = "y = einops.reduce(x, 'a b c -> a b', 'min')"
        elif method == "repeat":
            einops_method = f"einops_einops_{method}"
            snippet = "y = einops.repeat(x, 'a b c -> a b c d', d=2)"
        elif method == "rearrange":
            einops_method = f"einops_einops_{method}"
            snippet = "y = einops.rearrange(x, 'a b c -> a c b')"
        elif method == "einsum":
            einops_method = f"einops_einops_{method}"
            snippet = "y = einops.einsum(x, 'a b c -> a c b')"
        elif method == "pack":
            einops_method = f"einops_packing_{method}"
            snippet = "y, meta = einops.pack([x], '* b')"
        elif method == "unpack":
            einops_method = f"einops_packing_{method}"
            snippet = "x_packed, meta = einops.pack([x], '* b'); y = einops.unpack(x_packed, meta, '* b')[0]"
        else:
            self.fail(method)
        self._run_in_subprocess(flag, method, einops_method, snippet)

    @torch._dynamo.config.patch(enable_einops_tracing=True)
    def test_graph_break_message(self):
        @torch.compile(backend="eager", fullgraph=True)
        def fn(x):
            y = einops.reduce(x, "a b -> a", "min")
            return y.sin()

        x = torch.randn(2, 3, 4)  # force a graph break due to a shape mismatch
        self.assertExpectedInlineMunged(
            Unsupported,
            lambda: fn(x),
            """\
Failed to trace einops function
  Explanation: Failed to trace builtin operator
      Explanation: Dynamo does not know how to trace builtin operator `add` with argument types ['str', '<unknown type>'] (has_kwargs False)
      Hint: Avoid calling builtin `add` with argument types ['str', '<unknown type>']. Consider using an equivalent alternative function/method to `add`.
      Hint: If you are attempting to call a logging function (e.g. `print`), you can try adding it to `torch._dynamo.config.reorderable_logging_functions`.
      Hint: Please report an issue to PyTorch.

      Developer debug context: builtin add [<class 'torch._dynamo.variables.constant.ConstantVariable'>, <class 'torch._dynamo.variables.misc.StringFormatVariable'>] False

  Hint: Tracing through einops functions is experimental and may not be fully supported.
    To disable einops tracing, set `torch._dynamo.config.enable_einops_tracing = False`.
    Alternatively, explicitly allow this function in the graph with `torch._dynamo.allow_in_graph(reduce)`
  Hint: This is likely to be a Dynamo bug. Please report an issue to PyTorch.

  Developer debug context: einops function 'reduce'


from user code:
   File "test_einops.py", line N, in fn
    y = einops.reduce(x, "a b -> a", "min")""",  # noqa: B950
        )


instantiate_parametrized_tests(
    TestEinops,
)

if __name__ == "__main__":
    from torch._dynamo.test_case import run_tests

    run_tests()
