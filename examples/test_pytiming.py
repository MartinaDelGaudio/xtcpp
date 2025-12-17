import argparse
import time
from typing import Any, Dict

import numpy as np
import numpy.typing as npt
from mpi4py import MPI  # Required to call MPI_Init for us

import _xtcpp


def main() -> None:
    parser: argparse.ArgumentParser = argparse.ArgumentParser()
    parser.add_argument(
        "-b",
        "--batch_size",
        type=int,
        default=2,
        help="Batch size for SmallData writes.",
    )
    parser.add_argument(
        "-c",
        "--calib",
        action="store_true",
        help="If passed, use det.raw.calib. Otherwise det.raw.raw",
    )
    parser.add_argument(
        "-d",
        "--detector",
        type=str,
        default="jungfrau",
        help="Which detector to load.",
    )
    parser.add_argument(
        "--events_per_read",
        type=int,
        default=4000,
        help="Number of smd offsets (event offsets) to fetch per read.",
    )
    parser.add_argument(
        "-e",
        "--experiment",
        type=str,
        default="mfx101344525",
        help="Experiment to load",
    )
    parser.add_argument(
        "-r",
        "--run",
        type=int,
        default=70,
        help="Run to load",
    )
    parser.add_argument(
        "-s",
        "--smalldata",
        action="store_true",
        help="If passed, write a test HDF5 with SmallData.",
    )

    args: argparse.Namespace = parser.parse_args()

    rank: int = MPI.COMM_WORLD.Get_rank()

    pre_load_time: float = time.time()
    ds: _xtcpp.MPIDataSource = _xtcpp.MPIDataSource(
        args.experiment, args.run, args.events_per_read
    )
    batch_size: int = 2
    # small_data: _xtcpp.SmallData = _xtcpp.SmallData(batch_size)

    det: _xtcpp.DetectorWrapper = ds.detector(args.detector)
    end_load_time: float = time.time()

    print(f"[Rank {rank}] Beginning event loop")
    start_iterate_time: float = time.time()

    nevt: int = 0
    for nevt, evt in enumerate(ds):
        if args.calib and args.detector == "jungfrau":
            det_calib: npt.NDArray[np.float32] = det.raw.calib(evt)
            if args.smalldata:
                # SmallData takes a map of dataset to Any data
                # Currently, also have to pass a map of shapes
                dset_name: str = "/jungfrau/test"
                calib_roi: npt.NDArray[np.float32] = det_calib[0]
                event_data: Dict[str, Any] = {}
                event_data_shape: Dict[str, Any] = {}

                shape: npt.NDArray[np.uint16] = np.array(
                    calib_roi.shape, dtype=np.uint16
                )
                event_data[dset_name] = calib_roi
                event_data_shape[dset_name] = shape
                # small_data.event(event_data, event_data_shape)
        else:
            det_raw: Any = det.raw.raw(evt)
    end_iterate_time: float = time.time()

    total_load_time: float = end_load_time - pre_load_time
    total_run_time: float = end_iterate_time - start_iterate_time
    if nevt > 0:
        rate_iterate: float = nevt / total_run_time
        print(
            f"*** [Rank {rank}] Time elapsed:",
            total_run_time,
            "Rate:",
            rate_iterate,
            "Events/s -- Load time:",
            total_load_time,
        )


if __name__ == "__main__":
    main()
