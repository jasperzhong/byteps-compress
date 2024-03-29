# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================

import copy
import itertools
import unittest

import byteps.mxnet as bps
import mxnet as mx
import mxnet.ndarray as nd
import numpy as np
from gluoncv.model_zoo import get_model
from mxnet import autograd, gluon
from numba import jit
from parameterized import parameterized
from tqdm import tqdm

from meta_test import MetaTest
from utils import bernoulli, fake_data


@jit(nopython=True)
def round_next_pow2(v):
    v -= np.uint32(1)
    v |= v >> np.uint32(1)
    v |= v >> np.uint32(2)
    v |= v >> np.uint32(4)
    v |= v >> np.uint32(8)
    v |= v >> np.uint32(16)
    v += np.uint32(1)
    return v


def dithering(x, k, state, partition='linear', norm="max"):
    dtype = x.dtype
    y = x.flatten().astype(np.float32)
    if norm == "max":
        scale = np.max(np.abs(y))
    elif norm == "l2":
        scale = np.linalg.norm(y, ord=2)
    else:
        raise ValueError("Unsupported normalization")
    sign = np.array(0 < y).astype(np.int32) - np.array(y < 0).astype(np.int32)
    y = np.abs(y)
    y /= scale

    # stocastic rounding
    if partition == 'linear':
        y *= k
        low = np.floor(y)
        p = y - low  # whether to ceil
        y = low + bernoulli(p, state)
        y *= scale
        y /= k
    elif partition == "natural":
        y *= 2**(k-1)
        low = round_next_pow2((np.ceil(y).astype(np.uint32))) >> 1
        length = copy.deepcopy(low)
        length[length == 0] = 1
        p = (y - low) / length
        y = low + length * bernoulli(p, state)
        y = y.astype(np.float32)
        y *= scale
        y /= 2**(k-1)
    else:
        raise ValueError("Unsupported partition")

    y *= sign
    return y.reshape(x.shape).astype(dtype)


class DitheringTestCase(unittest.TestCase, metaclass=MetaTest):
    TEST_BENCH = [
        [2, 4, 8],
        ["linear", "natural"],
        ["l2"],
        ["float16"],
        np.random.randint(0, 2020, size=3).tolist()
    ]

    @parameterized.expand(itertools.product(*TEST_BENCH))
    def test_dithering(self, k, ptype, ntype, dtype, seed):
        ctx = mx.gpu(0)
        net = get_model("resnet18_v2")
        net.cast(dtype)
        net.initialize(mx.init.Xavier(), ctx=ctx)
        net.summary(nd.ones((1, 3, 224, 224),
                            ctx=ctx).astype(dtype, copy=False))

        # hyper-params
        batch_size = 32
        optimizer_params = {'momentum': 0, 'wd': 0,
                            'learning_rate': 0.01}

        compression_params = {
            "compressor": "dithering",
            "k": k,
            "partition": ptype,
            "normalize": ntype,
            "seed": seed,
            "fp16": True if dtype == "float16" else False
        }
        print(compression_params)

        trainer = bps.DistributedTrainer(net.collect_params(
        ), "sgd", optimizer_params, compression_params=compression_params)

        loss_fn = gluon.loss.SoftmaxCrossEntropyLoss()

        train_data = fake_data(batch_size=batch_size)

        params = {}
        rngs = {}
        rngs_s = {}

        for i, param in enumerate(trainer._params):
            if param.grad_req != 'null':
                params[i] = param._data[0].asnumpy()
                s = seed + i
                rngs[i] = np.array([s, s], dtype=np.uint64)
                rngs_s[i] = np.array([s, s], dtype=np.uint64)

        for it, batch in tqdm(enumerate(train_data)):
            data = batch[0].as_in_context(ctx).astype(dtype, copy=False)
            label = batch[1].as_in_context(ctx)

            with autograd.record():
                output = net(data)
                loss = loss_fn(output, label)

            loss.backward()

            gs = {}
            xs = {}

            for i, param in enumerate(trainer._params):
                if param.grad_req != 'null':
                    gs[i] = param._grad[0].asnumpy()
                    xs[i] = param._data[0].asnumpy()

            trainer.step(batch_size)

            for i, param in enumerate(trainer._params):
                if param.grad_req != "null":
                    g = gs[i] / (batch_size * bps.size())
                    c = dithering(g, k, rngs[i], ptype, ntype)

                    cs = dithering(c, k, rngs_s[i], ptype, ntype)
                    c = cs

                    params[i] -= optimizer_params["learning_rate"] * c

                    np_g = c.flatten()
                    mx_g = param._grad[0].asnumpy().flatten()
                    if not np.allclose(np_g, mx_g, atol=np.finfo(dtype).eps):
                        diff = np.abs(np_g - mx_g)
                        print("np", np_g)
                        print("mx", mx_g)
                        print("diff", diff)
                        print("max diff", np.max(diff))
                        idx = np.nonzero(diff > np.finfo(dtype).eps)
                        print("idx", idx, np_g[idx], mx_g[idx])
                        input()

        cnt = 0
        tot = 0
        threshold = 0 if dtype == "float32" else 10
        for i, param in enumerate(trainer._params):
            if param.grad_req != "null":
                x = param._data[0].asnumpy()
                tot += len(x.flatten())
                if not np.allclose(params[i], x, atol=np.finfo(dtype).eps):
                    diff = np.abs(x.flatten() - params[i].flatten())
                    idx = np.where(diff > np.finfo(dtype).eps)
                    cnt += len(idx[0])

        print("false/tot=%d/%d=%f" % (cnt, tot, cnt/tot))
        assert cnt <= threshold, "false/tot=%d/%d=%f" % (cnt, tot, cnt/tot)


if __name__ == '__main__':
    unittest.main()
