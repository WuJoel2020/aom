#!/usr/bin/env python
## Copyright (c) 2019, Alliance for Open Media. All rights reserved
##
## This source code is subject to the terms of the BSD 2 Clause License and
## the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
## was not distributed with this source code in the LICENSE file, you can
## obtain it at www.aomedia.org/license/software. If the Alliance for Open
## Media Patent License 1.0 was not distributed with this source code in the
## PATENTS file, you can obtain it at www.aomedia.org/license/patent.
##
__author__ = "maggie.sun@intel.com, ryan.lei@intel.com"

import numpy
import math
import scipy.interpolate
import logging
from Config import LoggerName

subloggername = "CalcBDRate"
loggername = LoggerName + '.' + '%s' % subloggername
logger = logging.getLogger(loggername)

# BJONTEGAARD    Bjontegaard metric
# Calculation is adapted from Google implementation
# PCHIP method - Piecewise Cubic Hermite Interpolating Polynomial interpolation
def BD_RATE(br1, qtyMtrc1, br2, qtyMtrc2):
    brqtypairs1 = [(br1[i], qtyMtrc1[i]) for i in range(min(len(qtyMtrc1), len(br1)))]
    brqtypairs2 = [(br2[i], qtyMtrc2[i]) for i in range(min(len(qtyMtrc2), len(br2)))]

    if not brqtypairs1 or not brqtypairs2:
        logger.info("one of input lists is empty!")
        return 0.0

    # sort the pair based on quality metric values increasing order
    brqtypairs1.sort(key=lambda tup: tup[1])
    brqtypairs2.sort(key=lambda tup: tup[1])

    logbr1 = [math.log(x[0]) for x in brqtypairs1]
    qmetrics1 = [100.0 if x[1] == float('inf') else x[1] for x in brqtypairs1]
    logbr2 = [math.log(x[0]) for x in brqtypairs2]
    qmetrics2 = [100.0 if x[1] == float('inf') else x[1] for x in brqtypairs2]

    # remove duplicated quality metric value
    dup_idx = [i for i in range(1, len(qmetrics1)) if qmetrics1[i - 1] == qmetrics1[i]]
    for idx in sorted(dup_idx, reverse=True):
        del qmetrics1[idx]
        del logbr1[idx]
    dup_idx = [i for i in range(1, len(qmetrics2)) if qmetrics2[i - 1] == qmetrics2[i]]
    for idx in sorted(dup_idx, reverse=True):
        del qmetrics2[idx]
        del logbr2[idx]

    # find max and min of quality metrics
    min_int = max(min(qmetrics1), min(qmetrics2))
    max_int = min(max(qmetrics1), max(qmetrics2))
    if min_int >= max_int:
        logger.info("no overlap from input 2 lists of quality metrics!")
        return 0.0

    # generate samples between max and min of quality metrics
    lin = numpy.linspace(min_int, max_int, num=100, retstep=True)
    interval = lin[1]
    samples = lin[0]

    # interpolation
    v1 = scipy.interpolate.pchip_interpolate(qmetrics1, logbr1, samples)
    v2 = scipy.interpolate.pchip_interpolate(qmetrics2, logbr2, samples)

    # Calculate the integral using the trapezoid method on the samples.
    int1 = numpy.trapz(v1, dx=interval)
    int2 = numpy.trapz(v2, dx=interval)

    # find avg diff
    avg_exp_diff = (int2 - int1) / (max_int - min_int)
    avg_diff = (math.exp(avg_exp_diff) - 1) * 100

    return avg_diff

'''
if __name__ == "__main__":
    brs1 = [64052.664, 6468.096, 4673.424, 3179.4, 2298.384, 1361.184]
    qtys1 = [1, 1, 0.99999, 0.99998, 0.99996, 0.99992]
    brs2 = [68461.896, 7554.96, 4827.432, 3294.024, 2380.128, 1401.744]
    qtys2 = [1, 1, 0.99999, 0.99998, 0.99996, 0.99992]

    bdrate = BD_RATE(brs1, qtys1, brs2, qtys2)
    print("bdrate calculated is %3.3f%%" % bdrate)
'''
