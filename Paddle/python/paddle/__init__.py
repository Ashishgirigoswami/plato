# Copyright (c) 2016 Paddle.python.paddlePaddle.python.paddle Authors. All Rights Reserved
#
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
import os
from Paddle.python.paddle.check_import_scipy import check_import_scipy

check_import_scipy(os.name)

try:
    from Paddle.python.paddle.version import full_version as __version__
    from Paddle.python.paddle.version import commit as __git_commit__

except ImportError:
    import sys
    sys.stderr.write('''Warning with import Paddle.python.paddle: you should not
     import Paddle.python.paddle from the source directory; please install Paddle.python.paddlePaddle.python.paddle*.whl firstly.'''
                     )

import Paddle.python.paddle.reader
import Paddle.python.paddle.dataset
import Paddle.python.paddle.batch
import Paddle.python.paddle.compat
import Paddle.python.paddle.distributed
batch = batch.batch
import Paddle.python.paddle.sysconfig
