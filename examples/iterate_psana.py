import argparse
import time
from typing import Union

import numpy as np
import numpy.typing as npt
from mpi4py import MPI

import psana

def main() -> None:
    parser: argparse.ArgumentParser = argparse.ArgumentParser()
    parser.add_argument(
        "-c",
        "--calib",
        action="store_true",
        help="If passed, use det.raw.calib. Otherwise det.raw.raw",
    )
    parser.add_argument(
        "--debug",
        action="store_true",
        help="If passed perform debug prints every 100 events (slows performance).",
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

    args: argparse.Namespace = parser.parse_args()

    start_load_time: int = time.time()
    ds: psana.psexp.mpi_ds.MPIDataSource = psana.DataSource(exp=args.experiment, run=args.run)
    myrun: psana.psexp.mpi_ds.RunParallel = next(ds.runs())
    epix100: "Container" = myrun.Detector("epix100_0")
    jungfrau: "Container" = myrun.Detector("jungfrau")

    end_load_time: int = time.time()
    total_load_time: float = float(end_load_time) - float(start_load_time)
    comm: MPI.Intracomm = MPI.COMM_WORLD
    rank: int = comm.Get_rank()
    size: int = comm.Get_size()

    print(f"[Rank {rank}] Entering event loop")

    start_time: int = 0
    nevt: int = 0
    evt: psana.event.Event
    for nevt, evt in enumerate(myrun.events()):
        epix_data: npt.NDArray[np.uint16] = epix100.raw.raw(evt)
        jungfrau_data: Union[npt.NDArray[np.float32], npt.NDArray[np.uint16]]
        if args.calib:
            jungfrau_data = jungfrau.raw.calib(evt)
        else:
            jungfrau_data = jungfrau.raw.raw(evt)
        if nevt == 0:
            start_time = time.time()

        if args.debug and (nevt % 100 == 0):
            print(f"[Rank {rank}] Processed event", nevt)

    end_time: int = time.time()
    total_time: int = end_time-start_time
    if nevt > 0:
        rate: float = (nevt - 1)/total_time
        print(f"*** [Rank {rank}] Time elapsed:", total_time, "Rate:", rate, "Events/s -- Load time:", total_load_time)

if __name__ == "__main__":
    main()
