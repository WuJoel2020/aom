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

import sys
import xlsxwriter
import xlrd
import argparse
from Config import VbaBinFile, QualityList, CalcBDRateInExcel
from CalcBDRate import BD_RATE


class ConvexHullData:
    ContentName = ""
    ContentClass = ""
    NumRDPoints = 0
    RDPoints = {}

    def __init__(self, Name="", Class="", num=0):
        self.ContentName = Name
        self.ContentClass = Class
        self.NumRDPoints = num
        self.RDPoints = {}


def ParseArguments(raw_args):
    parser = argparse.ArgumentParser(prog='ConvexHullBDRate.py',
                                     usage='%(prog)s [options]', description='')
    parser.add_argument('-i1', '--input1', dest='Input1', type=str,
                        required=True, metavar='',
                        help="convex hull summary excel file for base mode")
    parser.add_argument('-i2', '--input2', dest='Input2', type=str,
                        required=True, metavar='',
                        help="convex hull summary excel file for target mode")
    parser.add_argument('-o', '--output', dest='Output', type=str,
                        required=True, metavar='',
                        help="output excel file with BDRATE for base and target"
                             " modes")
    if len(raw_args) == 1:
        parser.print_help()
        sys.exit(1)
    args = parser.parse_args(raw_args[1:])

    global InputBase, InputTarget, Output
    InputBase = args.Input1
    InputTarget = args.Input2
    Output = args.Output


def read_cell_as_str(sht, row, col):
    cell_val = sht.cell(row, col).value
    if cell_val == '':
        return ''
    else:
        return str(cell_val)


def read_cell_as_float(sht, row, col):
    cell_val = sht.cell(row, col).value
    if cell_val == '':
        return ''
    else:
        return float(cell_val)


def read_cell_as_int(sht, row, col):
    cell_val = sht.cell(row, col).value
    if cell_val == '':
        return ''
    else:
        return int(cell_val)


def ParseConvexHullRD(xls):
    wb = xlrd.open_workbook(xls)
    shts = wb.sheet_names()   #list of sheet names
    data = {}   #dict of data, key is the sheet name

    cols = [3 + i * 4 for i in range(len(QualityList))]
    for sht_name in shts:
        sht = wb.sheet_by_name(sht_name)
        #skip the title row
        rows = sht.nrows
        start_row = 1
        while start_row < rows:
            row = start_row
            cls = read_cell_as_str(sht, row, 0)
            name = read_cell_as_str(sht, row, 1)
            num = read_cell_as_int(sht, row, 2)
            if cls == '' or name == '' or num == '':
                print("Error: read empty cells")
                exit()

            point = ConvexHullData(name, cls, num)
            rd_data = {}
            for row in range(num):
                for qty, col in zip(QualityList, cols):
                    res = read_cell_as_str(sht, start_row+row, col)         #Resolution
                    qp  = read_cell_as_int(sht, start_row+row, col + 1)     #QP
                    br  = read_cell_as_float(sht, start_row+row, col + 2)   #Bitrate
                    q   = read_cell_as_float(sht, start_row+row, col + 3)   #Quality
                    if br != '' and q != '':
                        if qty in rd_data:
                            rd_data[qty].append((res, qp, br, q))
                        else:
                            rd_data.update({qty: [(res, qp, br, q)]})

            start_row += num
            point.RDPoints = rd_data
            if sht_name in data:
                data[sht_name].append(point)
            else:
                data.update({sht_name: [point]})

    return shts, data


def WriteOutputHeaderRow(sht):
    sht.write(0, 0, 'Content Class')
    sht.write(0, 1, 'Content Name')
    sht.write(0, 2, 'Num RD Points')
    col = 3
    for qty in QualityList:
        sht.write(0, col, 'Resolution')
        sht.write(0, col + 1, 'QP')
        sht.write(0, col + 2, 'Bitrate(kbps)')
        sht.write(0, col + 3, qty)
        col += 4
    col += 1
    for qty in QualityList:
        sht.write(0, col, 'Resolution')
        sht.write(0, col + 1, 'QP')
        sht.write(0, col + 2, 'Bitrate(kbps)')
        sht.write(0, col + 3, qty)
        col += 4
    col += 1
    for (idx, qty) in zip(range(len(QualityList)), QualityList):
        sht.write(0, col + idx, "BDRATE-%s" % qty)


def WriteRDData(sht, rd_data, start_row, start_col, format):
    col = start_col
    max_rows = 0
    for qty in QualityList:
        row = start_row
        for (line, point) in zip(range(len(rd_data.RDPoints[qty])),
                                 rd_data.RDPoints[qty]):
            sht.write_string(row + line, col, point[0])                #Resolution
            sht.write_number(row + line, col + 1, point[1])            #QP
            sht.write_number(row + line, col + 2, point[2], format)    #Bitrate
            sht.write_number(row + line, col + 3, point[3], format)    #Quality
        col += 4
        max_rows = max(max_rows, len(rd_data.RDPoints[qty]))
    return max_rows


def WriteRDRecord(sht, base_data, target_data, start_row, bdrate_fmt, float_fmt):
    sht.write(start_row, 0, base_data.ContentClass)
    sht.write(start_row, 1, base_data.ContentName)

    #write base data
    base_start_col = 3
    base_max_rows = WriteRDData(sht, base_data, start_row, base_start_col,
                                float_fmt)

    #write target data
    target_start_col = base_start_col + 4 * len(QualityList) + 1
    target_max_rows = WriteRDData(sht, target_data, start_row, target_start_col,
                                  float_fmt)

    #write bdrate formula
    bdrate_start_col = target_start_col + 4 * len(QualityList) + 1
    total_rows = max(base_max_rows, target_max_rows)
    sht.write(start_row, 2, total_rows)
    for (qty, col) in zip(QualityList, range(len(QualityList))):
        if CalcBDRateInExcel:
            refbr_b = xlrd.cellnameabs(start_row, base_start_col + col * 4 + 2)
            refbr_e = xlrd.cellnameabs(start_row + total_rows - 1,
                                       base_start_col + col * 4 + 2)
            refq_b = xlrd.cellnameabs(start_row, base_start_col + col * 4 + 3)
            refq_e = xlrd.cellnameabs(start_row + total_rows - 1,
                                      base_start_col + col * 4 + 3)

            testbr_b = xlrd.cellnameabs(start_row, target_start_col + col * 4 + 2)
            testbr_e = xlrd.cellnameabs(start_row + total_rows - 1,
                                        target_start_col + col * 4 + 2)
            testq_b = xlrd.cellnameabs(start_row, target_start_col + col * 4 + 3)
            testq_e = xlrd.cellnameabs(start_row + total_rows - 1,
                                       target_start_col + col * 4 + 3)

            # formula = '=-bdrate(%s:%s,%s:%s,%s:%s,%s:%s)' % (
            # refbr_b, refbr_e, refq_b, refq_e, testbr_b, testbr_e, testq_b, testq_e)
            formula = '=bdRateExtend(%s:%s,%s:%s,%s:%s,%s:%s)'\
                % (refbr_b, refbr_e, refq_b, refq_e, testbr_b, testbr_e, testq_b, testq_e)
            sht.write_formula(start_row, bdrate_start_col + col, formula, bdrate_fmt)
        else:
            refbrs   = [base_data.RDPoints[qty][i][2] for i in range(len(base_data.RDPoints[qty]))]
            refqtys  = [base_data.RDPoints[qty][i][3] for i in range(len(base_data.RDPoints[qty]))]
            testbrs  = [target_data.RDPoints[qty][i][2] for i in range(len(target_data.RDPoints[qty]))]
            testqtys = [target_data.RDPoints[qty][i][3] for i in range(len(target_data.RDPoints[qty]))]
            bdrate = BD_RATE(refbrs, refqtys, testbrs, testqtys) / 100.0
            sht.write_number(start_row, bdrate_start_col + col, bdrate, bdrate_fmt)
    return total_rows


def FindContent(name, rd_data):
    for data in rd_data:
        if name == data.ContentName:
            return data
    return ''

######################################
# main
######################################
if __name__ == "__main__":
    #sys.argv = ["","-i1","ConvexHullData_ScaleAlgosNum_6_aom_av1_1.xlsx",
    #"-i2","ConvexHullData_ScaleAlgosNum_6_aom_av1_6.xlsx",
    #"-o","ConvexHullBDRate.xlsm"]
    ParseArguments(sys.argv)

    base_shts, base_rd_data = ParseConvexHullRD(InputBase)
    target_shts, target_rd_data = ParseConvexHullRD(InputTarget)

    output_wb = xlsxwriter.Workbook(Output)
    # vba file needed when to calculate bdrate
    output_wb.add_vba_project(VbaBinFile)
    bdrate_fmt = output_wb.add_format()
    bdrate_fmt.set_num_format('0.00%')
    float_fmt = output_wb.add_format()
    float_fmt.set_num_format('0.00')

    for sht_name in base_shts:
        if sht_name in target_shts:
            sht = output_wb.add_worksheet(sht_name)
            WriteOutputHeaderRow(sht)
            start_row = 1
            for base_data in base_rd_data[sht_name]:
                ContentName = base_data.ContentName
                target_data = FindContent(ContentName, target_rd_data[sht_name])
                if target_data != '':
                    total_rows = WriteRDRecord(sht, base_data, target_data,
                                               start_row, bdrate_fmt, float_fmt)
                    start_row += total_rows

    output_wb.close()
