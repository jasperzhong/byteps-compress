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

from meta_test import MetaTests
from utils import fake_data, randint


def randomk(x, k, state):
    y = x.flatten()
    low = np.uint64(0)
    high = np.uint64(len(y))
    indices = np.array([randint(low, high, state)
                        for _ in range(k)], dtype=np.uint64)
    vals = y[indices]
    y.fill(0)
    scale = len(y) / k
    for idx, val in zip(indices, vals):
        y[idx] = val * scale
    return y.reshape(x.shape)


class RandomkTestCase(unittest.TestCase, metaclass=MetaTest):
    TEST_BENCH = [
        [1, 3, 5],
        ["float32", "float16"],
        np.random.randint(0, 2020, size=3).tolist()
    ]

    @parameterized.expand(itertools.product(*TEST_BENCH))
    def test_randomk(self, k, dtype, seed):
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
            "compressor": "randomk",
            "k": k,
            "seed": seed,
            "fp16": True if dtype == "float16" else False
        }

        params = net.collect_params()
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
                s = seed + i + k
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
                    c = randomk(g, k, rngs[i])

                    params[i] -= optimizer_params["learning_rate"] * c

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

        assert cnt <= threshold, "false/tot=%d/%d=%f" % (cnt, tot, cnt/tot)


if __name__ == '__main__':
    unittest.main()
