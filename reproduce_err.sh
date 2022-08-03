#!/bin/bash

#CNTOOLKIT_VERSION="3.0.1"
#CNNL_VERSION="1.11.1"
#CNNL_EXTRA_VERSION="0.17.0"
#CNLIGHT_VERSION="0.15.0"
# 10.100.146.25 MLU290

cd bangc-ops
mkdir - p err_build
cd err_build
cmake .. -DNEUWARE_HOME=/usr/local/neuware

make -j8
cd test
./mluop_gtest --gtest_filter=*poly_nms*/3