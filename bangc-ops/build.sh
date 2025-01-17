#!/bin/bash
set -e

SCRIPT_DIR=`dirname $0`
BUILD_PATH=${SCRIPT_DIR}/build
CMAKE=cmake

usage () {
    echo "USAGE: ./build.sh <options>"
    echo
    echo "OPTIONS:"
    echo "      -h, --help         Print usage."
    echo "      -c, --coverage     Build mluops with coverage test."
    echo
}

cmdline_args=$(getopt -o ch -n 'build.sh' -- "$@")
eval set -- "$cmdline_args"

if [ $# != 0 ]; then
  while true; do
    case "$1" in
      -c | --coverage)
          shift
          export MLUOP_BUILD_COVERAGE_TEST="ON"
          ;;
      --asan)
          shift
          export MLUOP_BUILD_ASAN_CHECK="ON"
          ;;
      -h | --help)
          usage
          exit 0
          ;;
      --)
          shift
          break
          ;;
      *)
          echo "-- Unknown options ${1}, use -h or --help"
          usage
          exit -1
          ;;
    esac
  done
fi

if [ ! -d "$BUILD_PATH" ]; then
  mkdir "$BUILD_PATH"
fi

if [ ! -z "${NEUWARE_HOME}" ]; then
  echo "-- using NEUWARE_HOME = ${NEUWARE_HOME}"
else
  echo "-- NEUWARE_HOME is null, refer README.md to prepare NEUWARE_HOME environment."
  exit -1
fi

pushd ${BUILD_PATH} > /dev/null
  rm -rf *
  if [[ ${MLUOP_BUILD_COVERAGE_TEST} == "ON" ]]; then
    echo "-- Build cambricon coverage test cases."
    ${CMAKE}  ../ -DNEUWARE_HOME="${NEUWARE_HOME}" -DMLUOP_BUILD_COVERAGE_TEST="${MLUOP_BUILD_COVERAGE_TEST}"
  else
    echo "-- Build cambricon release test cases."
    ${CMAKE}  ../ -DNEUWARE_HOME="${NEUWARE_HOME}"
  fi 

  if [[ ${MLUOP_BUILD_ASAN_CHECK} == "ON" ]]; then
    echo "-- Build cambricon ASAN leak check."
    ${CMAKE}  ../ -DNEUWARE_HOME="${NEUWARE_HOME}" -DMLUOP_BUILD_ASAN_CHECK="${MLUOP_BUILD_ASAN_CHECK}"
  fi
popd > /dev/null
${CMAKE} --build build --  -j
