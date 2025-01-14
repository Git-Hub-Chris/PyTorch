# Owner(s): ["module: dynamo"]
import unittest

import torch
import torch._dynamo.test_case
from torch._dispatch.python import enable_python_dispatcher
from torch._dynamo.testing import CompileCounter, EagerAndRecordGraphs, normalize_gm
from torch.testing._internal.common_cuda import TEST_CUDA


class PythonDispatcherTests(torch._dynamo.test_case.TestCase):
    def test_dispatch_key(self):
        eager = EagerAndRecordGraphs()

        @torch.compile(backend=eager, fullgraph=True)
        def fn(x):
            key_set = torch._C._dispatch_tls_local_include_set()
            key_set = key_set | torch._C._dispatch_keys(x)
            key_set = key_set - torch._C._dispatch_tls_local_exclude_set()
            if key_set.highestPriorityTypeId() == torch.DispatchKey.PythonDispatcher:
                return torch.sin(x + 1)
            else:
                return torch.sin(x - 1)

        x = torch.randn(2, 3)
        with enable_python_dispatcher():
            self.assertEqual(fn(x), torch.sin(x + 1))

        graph = eager.graphs[0]
        actual = normalize_gm(graph.print_readable(False))

        self.assertExpectedInline(
            actual,
            """\
class GraphModule(torch.nn.Module):
    def forward(self, L_x_: "f32[2, 3]"):
        l_x_ = L_x_

        add: "f32[2, 3]" = l_x_ + 1;  l_x_ = None
        sin: "f32[2, 3]" = torch.sin(add);  add = None
        return (sin,)
""",  # NOQA: B950
        )

    @unittest.skipIf(not TEST_CUDA, "requires cuda")
    def test_dispatch_key_set_guard(self):
        counter = CompileCounter()

        @torch.compile(backend=counter, fullgraph=True)
        def fn(x, dks):
            if dks.has("CPU"):
                return torch.sin(x + 1)
            else:
                return torch.sin(x - 1)

        x1 = torch.randn(2, 3)
        dks1 = torch._C._dispatch_keys(x1)
        self.assertEqual(fn(x1, dks1), torch.sin(x1 + 1))
        self.assertEqual(counter.frame_count, 1)

        x2 = torch.randn(2, 3)
        dks2 = torch._C._dispatch_keys(x2)
        self.assertEqual(fn(x2, dks2), torch.sin(x2 + 1))
        # No recompile since the dispatch key set is the same though the tensor is different.
        self.assertEqual(counter.frame_count, 1)

        x3 = torch.randn(2, 3, device="cuda")
        dks3 = torch._C._dispatch_keys(x3)
        self.assertEqual(fn(x3, dks3), torch.sin(x3 - 1))
        # Re-compile since the dispatch key set is different.
        self.assertEqual(counter.frame_count, 2)


if __name__ == "__main__":
    from torch._dynamo.test_case import run_tests

    run_tests()
