#   Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
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
"""
TestCases for Dataset,
including create, config, run, etc.
"""

from __future__ import print_function
import paddle.fluid as fluid
import paddle.compat as cpt
import paddle.fluid.core as core
import numpy as np
import os
import shutil
import unittest


class TestDataset(unittest.TestCase):
    """  TestCases for Dataset. """

    def setUp(self):
        self.use_data_loader = False
        self.epoch_num = 10
        self.drop_last = False

    def test_dataset_create(self):
        """ Testcase for dataset create. """
        try:
            dataset = fluid.DatasetFactory().create_dataset("InMemoryDataset")
        except:
            self.assertTrue(False)

        try:
            dataset = fluid.DatasetFactory().create_dataset("QueueDataset")
        except:
            self.assertTrue(False)

        try:
            dataset = fluid.DatasetFactory().create_dataset(
                "FileInstantDataset")
        except:
            self.assertTrue(False)

        try:
            dataset = fluid.DatasetFactory().create_dataset("MyOwnDataset")
            self.assertTrue(False)
        except:
            self.assertTrue(True)

    def test_config(self):
        """
        Testcase for python config.
        """
        dataset = fluid.InMemoryDataset()
        dataset.set_parse_ins_id(True)
        dataset.set_parse_content(True)
        self.assertTrue(dataset.parse_ins_id)
        self.assertTrue(dataset.parse_content)

    def test_run_with_dump(self):
        """
        Testcase for InMemoryDataset from create to run.
        """
        with open("test_run_with_dump_a.txt", "w") as f:
            data = "1 a 1 a 1 1 2 3 3 4 5 5 5 5 1 1\n"
            data += "1 b 1 b 1 2 2 3 4 4 6 6 6 6 1 2\n"
            data += "1 c 1 c 1 3 2 3 5 4 7 7 7 7 1 3\n"
            f.write(data)
        with open("test_run_with_dump_b.txt", "w") as f:
            data = "1 d 1 d 1 4 2 3 3 4 5 5 5 5 1 4\n"
            data += "1 e 1 e 1 5 2 3 4 4 6 6 6 6 1 5\n"
            data += "1 f 1 f 1 6 2 3 5 4 7 7 7 7 1 6\n"
            data += "1 g 1 g 1 7 2 3 6 4 8 8 8 8 1 7\n"
            f.write(data)

        slots = ["slot1", "slot2", "slot3", "slot4"]
        slots_vars = []
        for slot in slots:
            var = fluid.layers.data(
                name=slot, shape=[1], dtype="int64", lod_level=1)
            slots_vars.append(var)

        dataset = fluid.DatasetFactory().create_dataset("InMemoryDataset")
        dataset.set_batch_size(32)
        dataset.set_thread(3)
        dataset.set_filelist(
            ["test_run_with_dump_a.txt", "test_run_with_dump_b.txt"])
        dataset.set_parse_ins_id(True)
        dataset.set_parse_content(True)
        dataset.set_pipe_command("cat")
        dataset.set_use_var(slots_vars)
        dataset.load_into_memory()
        dataset.set_fea_eval(10000, True)
        dataset.local_shuffle()

        exe = fluid.Executor(fluid.CPUPlace())
        exe.run(fluid.default_startup_program())
        for i in range(2):
            try:
                exe.train_from_dataset(fluid.default_main_program(), dataset)
            except ImportError as e:
                pass
            except Exception as e:
                self.assertTrue(False)

        os.remove("./test_run_with_dump_a.txt")
        os.remove("./test_run_with_dump_b.txt")

    def test_dataset_config(self):
        """ Testcase for dataset configuration. """
        dataset = fluid.core.Dataset("MultiSlotDataset")
        dataset.set_thread_num(12)
        dataset.set_filelist(["a.txt", "b.txt", "c.txt"])
        dataset.set_trainer_num(4)
        dataset.set_hdfs_config("my_fs_name", "my_fs_ugi")

        thread_num = dataset.get_thread_num()
        self.assertEqual(thread_num, 12)

        filelist = dataset.get_filelist()
        self.assertEqual(len(filelist), 3)
        self.assertEqual(filelist[0], "a.txt")
        self.assertEqual(filelist[1], "b.txt")
        self.assertEqual(filelist[2], "c.txt")

        trainer_num = dataset.get_trainer_num()
        self.assertEqual(trainer_num, 4)

        name, ugi = dataset.get_hdfs_config()
        self.assertEqual(name, "my_fs_name")
        self.assertEqual(ugi, "my_fs_ugi")

    def test_in_memory_dataset_run(self):
        """
        Testcase for InMemoryDataset from create to run.
        """
        with open("test_in_memory_dataset_run_a.txt", "w") as f:
            data = "1 1 2 3 3 4 5 5 5 5 1 1\n"
            data += "1 2 2 3 4 4 6 6 6 6 1 2\n"
            data += "1 3 2 3 5 4 7 7 7 7 1 3\n"
            f.write(data)
        with open("test_in_memory_dataset_run_b.txt", "w") as f:
            data = "1 4 2 3 3 4 5 5 5 5 1 4\n"
            data += "1 5 2 3 4 4 6 6 6 6 1 5\n"
            data += "1 6 2 3 5 4 7 7 7 7 1 6\n"
            data += "1 7 2 3 6 4 8 8 8 8 1 7\n"
            f.write(data)

        slots = ["slot1", "slot2", "slot3", "slot4"]
        slots_vars = []
        for slot in slots:
            var = fluid.layers.data(
                name=slot, shape=[1], dtype="int64", lod_level=1)
            slots_vars.append(var)

        dataset = fluid.DatasetFactory().create_dataset("InMemoryDataset")
        dataset.set_batch_size(32)
        dataset.set_thread(3)
        dataset.set_filelist([
            "test_in_memory_dataset_run_a.txt",
            "test_in_memory_dataset_run_b.txt"
        ])
        dataset.set_pipe_command("cat")
        dataset.set_use_var(slots_vars)
        dataset.load_into_memory()
        dataset.set_fea_eval(10000, True)
        dataset.slots_shuffle(["slot1"])
        dataset.local_shuffle()

        exe = fluid.Executor(fluid.CPUPlace())
        exe.run(fluid.default_startup_program())
        if self.use_data_loader:
            data_loader = fluid.io.DataLoader.from_dataset(dataset,
                                                           fluid.cpu_places(),
                                                           self.drop_last)
            for i in range(self.epoch_num):
                for data in data_loader():
                    exe.run(fluid.default_main_program(), feed=data)
        else:
            for i in range(self.epoch_num):
                try:
                    exe.train_from_dataset(fluid.default_main_program(),
                                           dataset)
                except Exception as e:
                    self.assertTrue(False)

        os.remove("./test_in_memory_dataset_run_a.txt")
        os.remove("./test_in_memory_dataset_run_b.txt")

    def test_in_memory_dataset_run_2(self):
        """
        Testcase for InMemoryDataset from create to run.
        Use CUDAPlace
        Use float type id
        """
        with open("test_in_memory_dataset_run_a.txt", "w") as f:
            data = "1 1 2 3 3 4 5 5 5 5 1 1\n"
            data += "1 2 2 3 4 4 6 6 6 6 1 2\n"
            data += "1 3 2 3 5 4 7 7 7 7 1 3\n"
            f.write(data)
        with open("test_in_memory_dataset_run_b.txt", "w") as f:
            data = "1 4 2 3 3 4 5 5 5 5 1 4\n"
            data += "1 5 2 3 4 4 6 6 6 6 1 5\n"
            data += "1 6 2 3 5 4 7 7 7 7 1 6\n"
            data += "1 7 2 3 6 4 8 8 8 8 1 7\n"
            f.write(data)

        slots = ["slot1_f", "slot2_f", "slot3_f", "slot4_f"]
        slots_vars = []
        for slot in slots:
            var = fluid.layers.data(
                name=slot, shape=[1], dtype="float32", lod_level=1)
            slots_vars.append(var)

        dataset = fluid.DatasetFactory().create_dataset("InMemoryDataset")
        dataset.set_batch_size(32)
        dataset.set_thread(3)
        dataset.set_filelist([
            "test_in_memory_dataset_run_a.txt",
            "test_in_memory_dataset_run_b.txt"
        ])
        dataset.set_pipe_command("cat")
        dataset.set_use_var(slots_vars)
        dataset.load_into_memory()
        dataset.local_shuffle()

        exe = fluid.Executor(fluid.CPUPlace() if not core.is_compiled_with_cuda(
        ) else fluid.CUDAPlace(0))
        exe.run(fluid.default_startup_program())

        for i in range(2):
            try:
                exe.train_from_dataset(fluid.default_main_program(), dataset)
                exe.train_from_dataset(
                    fluid.default_main_program(), dataset, thread=1)
                exe.train_from_dataset(
                    fluid.default_main_program(), dataset, thread=2)
                exe.train_from_dataset(
                    fluid.default_main_program(), dataset, thread=2)
                exe.train_from_dataset(
                    fluid.default_main_program(), dataset, thread=3)
                exe.train_from_dataset(
                    fluid.default_main_program(), dataset, thread=4)
            except ImportError as e:
                pass
            except Exception as e:
                self.assertTrue(False)

        if self.use_data_loader:
            data_loader = fluid.io.DataLoader.from_dataset(dataset,
                                                           fluid.cpu_places(),
                                                           self.drop_last)
            for i in range(self.epoch_num):
                for data in data_loader():
                    exe.run(fluid.default_main_program(), feed=data)
        else:
            for i in range(self.epoch_num):
                try:
                    exe.train_from_dataset(fluid.default_main_program(),
                                           dataset)
                except Exception as e:
                    self.assertTrue(False)

        dataset.set_merge_by_lineid(2)
        dataset.set_parse_ins_id(False)
        dataset.set_fleet_send_sleep_seconds(2)
        dataset.preload_into_memory()
        dataset.wait_preload_done()
        dataset.release_memory()
        dataset.preload_into_memory(1)
        dataset.wait_preload_done()
        fleet_ptr = fluid.core.Fleet()
        fleet_ptr.set_client2client_config(1, 1, 1)

        os.remove("./test_in_memory_dataset_run_a.txt")
        os.remove("./test_in_memory_dataset_run_b.txt")

    def test_queue_dataset_run(self):
        """
        Testcase for QueueDataset from create to run.
        """
        with open("test_queue_dataset_run_a.txt", "w") as f:
            data = "1 1 2 3 3 4 5 5 5 5 1 1\n"
            data += "1 2 2 3 4 4 6 6 6 6 1 2\n"
            data += "1 3 2 3 5 4 7 7 7 7 1 3\n"
            f.write(data)
        with open("test_queue_dataset_run_b.txt", "w") as f:
            data = "1 4 2 3 3 4 5 5 5 5 1 4\n"
            data += "1 5 2 3 4 4 6 6 6 6 1 5\n"
            data += "1 6 2 3 5 4 7 7 7 7 1 6\n"
            data += "1 7 2 3 6 4 8 8 8 8 1 7\n"
            f.write(data)

        slots = ["slot1", "slot2", "slot3", "slot4"]
        slots_vars = []
        for slot in slots:
            var = fluid.layers.data(
                name=slot, shape=[1], dtype="int64", lod_level=1)
            slots_vars.append(var)

        dataset = fluid.DatasetFactory().create_dataset("QueueDataset")
        dataset.set_batch_size(32)
        dataset.set_thread(3)
        dataset.set_filelist(
            ["test_queue_dataset_run_a.txt", "test_queue_dataset_run_b.txt"])
        dataset.set_pipe_command("cat")
        dataset.set_use_var(slots_vars)

        exe = fluid.Executor(fluid.CPUPlace())
        exe.run(fluid.default_startup_program())
        if self.use_data_loader:
            data_loader = fluid.io.DataLoader.from_dataset(dataset,
                                                           fluid.cpu_places(),
                                                           self.drop_last)
            for i in range(self.epoch_num):
                for data in data_loader():
                    exe.run(fluid.default_main_program(), feed=data)
        else:
            for i in range(self.epoch_num):
                try:
                    exe.train_from_dataset(fluid.default_main_program(),
                                           dataset)
                except Exception as e:
                    self.assertTrue(False)

        dataset2 = fluid.DatasetFactory().create_dataset("QueueDataset")
        dataset2.set_use_var(slots_vars)
        dataset2.set_batch_size(32)
        dataset2.set_thread(3)
        dataset2.set_pipe_command("cat")
        dataset.set_filelist([])
        try:
            exe.train_from_dataset(fluid.default_main_program(), dataset2)
        except ImportError as e:
            print("warning: we skip trainer_desc_pb2 import problem in windows")
        except Exception as e:
            self.assertTrue(False)

        os.remove("./test_queue_dataset_run_a.txt")
        os.remove("./test_queue_dataset_run_b.txt")

    def test_queue_dataset_run_2(self):
        """
        Testcase for QueueDataset from create to run.
        Use CUDAPlace
        Use float type id
        """
        with open("test_queue_dataset_run_a.txt", "w") as f:
            data = "1 1 2 3 3 4 5 5 5 5 1 1\n"
            data += "1 2 2 3 4 4 6 6 6 6 1 2\n"
            data += "1 3 2 3 5 4 7 7 7 7 1 3\n"
            f.write(data)
        with open("test_queue_dataset_run_b.txt", "w") as f:
            data = "1 4 2 3 3 4 5 5 5 5 1 4\n"
            data += "1 5 2 3 4 4 6 6 6 6 1 5\n"
            data += "1 6 2 3 5 4 7 7 7 7 1 6\n"
            data += "1 7 2 3 6 4 8 8 8 8 1 7\n"
            f.write(data)

        slots = ["slot1_f", "slot2_f", "slot3_f", "slot4_f"]
        slots_vars = []
        for slot in slots:
            var = fluid.layers.data(
                name=slot, shape=[1], dtype="float32", lod_level=1)
            slots_vars.append(var)

        dataset = fluid.DatasetFactory().create_dataset("QueueDataset")
        dataset.set_batch_size(32)
        dataset.set_thread(3)
        dataset.set_filelist(
            ["test_queue_dataset_run_a.txt", "test_queue_dataset_run_b.txt"])
        dataset.set_pipe_command("cat")
        dataset.set_use_var(slots_vars)

        exe = fluid.Executor(fluid.CPUPlace() if not core.is_compiled_with_cuda(
        ) else fluid.CUDAPlace(0))
        exe.run(fluid.default_startup_program())
        if self.use_data_loader:
            data_loader = fluid.io.DataLoader.from_dataset(dataset,
                                                           fluid.cpu_places(),
                                                           self.drop_last)
            for i in range(self.epoch_num):
                for data in data_loader():
                    exe.run(fluid.default_main_program(), feed=data)
        else:
            for i in range(self.epoch_num):
                try:
                    exe.train_from_dataset(fluid.default_main_program(),
                                           dataset)
                except Exception as e:
                    self.assertTrue(False)

        os.remove("./test_queue_dataset_run_a.txt")
        os.remove("./test_queue_dataset_run_b.txt")


class TestDatasetWithDataLoader(TestDataset):
    def setUp(self):
        self.use_data_loader = True
        self.epoch_num = 10
        self.drop_last = False


class TestDatasetWithFetchHandler(unittest.TestCase):
    def net(self):
        slots = ["slot1", "slot2", "slot3", "slot4"]
        slots_vars = []
        poolings = []
        for slot in slots:
            data = fluid.layers.data(
                name=slot, shape=[1], dtype="int64", lod_level=1)
            var = fluid.layers.cast(x=data, dtype='float32')
            pool = fluid.layers.sequence_pool(input=var, pool_type='AVERAGE')

            slots_vars.append(data)
            poolings.append(pool)

        concated = fluid.layers.concat(poolings, axis=1)
        fc = fluid.layers.fc(input=concated, act='tanh', size=32)
        return slots_vars, fc

    def get_dataset(self, inputs, files):
        dataset = fluid.DatasetFactory().create_dataset("QueueDataset")
        dataset.set_batch_size(32)
        dataset.set_thread(3)
        dataset.set_filelist(files)
        dataset.set_pipe_command("cat")
        dataset.set_use_var(inputs)
        return dataset

    def setUp(self):
        with open("test_queue_dataset_run_a.txt", "w") as f:
            data = "1 1 2 3 3 4 5 5 5 5 1 1\n"
            data += "1 2 2 3 4 4 6 6 6 6 1 2\n"
            data += "1 3 2 3 5 4 7 7 7 7 1 3\n"
            f.write(data)
        with open("test_queue_dataset_run_b.txt", "w") as f:
            data = "1 4 2 3 3 4 5 5 5 5 1 4\n"
            data += "1 5 2 3 4 4 6 6 6 6 1 5\n"
            data += "1 6 2 3 5 4 7 7 7 7 1 6\n"
            data += "1 7 2 3 6 4 8 8 8 8 1 7\n"
            f.write(data)

    def tearDown(self):
        os.remove("./test_queue_dataset_run_a.txt")
        os.remove("./test_queue_dataset_run_b.txt")

    def test_dataset_none(self):
        slots_vars, out = self.net()
        files = ["test_queue_dataset_run_a.txt", "test_queue_dataset_run_b.txt"]
        dataset = self.get_dataset(slots_vars, files)

        exe = fluid.Executor(fluid.CPUPlace())
        exe.run(fluid.default_startup_program())

        # test dataset->None
        try:
            exe.train_from_dataset(fluid.default_main_program(), None)
        except ImportError as e:
            print("warning: we skip trainer_desc_pb2 import problem in windows")
        except RuntimeError as e:
            error_msg = "dataset is need and should be initialized"
            self.assertEqual(error_msg, cpt.get_exception_message(e))
        except Exception as e:
            self.assertTrue(False)

    def test_infer_from_dataset(self):
        slots_vars, out = self.net()
        files = ["test_queue_dataset_run_a.txt", "test_queue_dataset_run_b.txt"]
        dataset = self.get_dataset(slots_vars, files)

        exe = fluid.Executor(fluid.CPUPlace())
        exe.run(fluid.default_startup_program())

        try:
            exe.infer_from_dataset(fluid.default_main_program(), dataset)
        except ImportError as e:
            print("warning: we skip trainer_desc_pb2 import problem in windows")
        except Exception as e:
            self.assertTrue(False)

    def test_fetch_handler(self):
        slots_vars, out = self.net()
        files = ["test_queue_dataset_run_a.txt", "test_queue_dataset_run_b.txt"]
        dataset = self.get_dataset(slots_vars, files)

        exe = fluid.Executor(fluid.CPUPlace())
        exe.run(fluid.default_startup_program())

        fh = fluid.executor.FetchHandler(out.name)
        fh.help()

        try:
            exe.train_from_dataset(
                program=fluid.default_main_program(),
                dataset=dataset,
                fetch_handler=fh)
        except ImportError as e:
            print("warning: we skip trainer_desc_pb2 import problem in windows")
        except RuntimeError as e:
            error_msg = "dataset is need and should be initialized"
            self.assertEqual(error_msg, cpt.get_exception_message(e))
        except Exception as e:
            self.assertTrue(False)


if __name__ == '__main__':
    unittest.main()
