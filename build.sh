#!/usr/bin/env bash
mkdir -p install

if [[ $HOSTNAME =~ "sdf" ]]; then
    source /sdf/group/lcls/ds/ana/sw/conda2/manage/bin/psconda.sh
fi

BASE_DIR="$( readlink -f "$( dirname "${BASH_SOURCE[0]}" )" )"
INSTALL_DIR="${BASE_DIR}/install"

export CC=$(which mpicc)
export CXX=$(which mpicxx)
#export CXX="/sdf/group/lcls/ds/ana/sw/conda2/inst/envs/ps_20241122/bin/mpicxx"

echo "Will build an installation of XTCPP at ${INSTALL_DIR}"

pip install . --prefix="${INSTALL_DIR}" --no-dependencies
