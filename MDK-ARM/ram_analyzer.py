#!/usr/bin/env python3
"""
STM32 RAM 使用分析脚本
======================
两种模式：
  1. --map    解析 Keil .map 文件（精确数据）
  2. --src    扫描 C/H 源文件，估算全局/静态变量 RAM 占用

用法：
  python ram_analyzer.py --map test.map
  python ram_analyzer.py --src ../Core
  python ram_analyzer.py --map test.map --src ../Core
"""

import re
import os
import sys
import struct
import argparse
from pathlib import Path
from collections import defaultdict

# ============================================================
# 常见类型大小表 (ARM Cortex-M, 32-bit, 默认对齐)
# ============================================================
TYPE_SIZES = {
    # 标准 C 类型
    'char':          1, 'unsigned char':      1, 'uint8_t':    1, 'int8_t':    1,
    'short':         2, 'unsigned short':     2, 'uint16_t':   2, 'int16_t':   2,
    'int':           4, 'unsigned int':       4, 'uint32_t':   4, 'int32_t':   4,
    'long':          4, 'unsigned long':      4,
    'long long':     8, 'unsigned long long': 8, 'uint64_t':   8, 'int64_t':   8,
    'float':         4, 'double':             8,
    'void*':         4, 'char*':              4,
    # HAL 句柄
    'UART_HandleTypeDef': 84, 'SPI_HandleTypeDef':  76,
    'DMA_HandleTypeDef':   44, 'I2C_HandleTypeDef':  84,
    'TIM_HandleTypeDef':   76, 'ADC_HandleTypeDef':  92,
    'GPIO_InitTypeDef':    20, 'FLASH_EraseInitTypeDef': 16,
    # LFS
    'lfs_t':             120, 'lfs_file_t':       100,
    'lfs_dir_t':          60, 'lfs_config':       60,
}

# 正则：匹配数组声明中的维度，如 [40960] [5][64]
RE_ARRAY_DIM = re.compile(r'\[(\d+)\]')


def get_type_size(type_str):
    """根据类型字符串估算大小"""
    type_str = type_str.strip()
    if type_str in TYPE_SIZES:
        return TYPE_SIZES[type_str]
    # 指针
    if type_str.endswith('*'):
        return 4
    # enum -> 4
    if 'enum' in type_str:
        return 4
    # struct -> 粗略按名称查找，找不到给 4
    if 'struct' in type_str or '_t' in type_str:
        return TYPE_SIZES.get(type_str, 4)
    return 4


def calc_var_size(decl_str, type_size):
    """计算变量总大小 = 类型大小 × 各维度之积"""
    dims = RE_ARRAY_DIM.findall(decl_str)
    total = type_size
    for d in dims:
        total *= int(d)
    return total


# ============================================================
# 模式 1：解析 .map 文件
# ============================================================
def parse_map(filepath):
    """解析 Keil ARM Compiler .map 文件"""
    with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
        content = f.read()

    # 去 ANSI 转义码
    content = re.sub(r'\x1b\[[0-9;]*m', '', content)

    results = {
        'regions': {},
        'modules': [],
        'grand_totals': {},
    }

    # --- Execution Region ---
    for m in re.finditer(
        r'Execution Region\s+(\w+)\s+\(Exec base:\s+(0x[0-9A-Fa-f]+),\s+'
        r'Load base:\s+(0x[0-9A-Fa-f]+),\s+Size:\s+(0x[0-9A-Fa-f]+),\s+'
        r'Max:\s+(0x[0-9A-Fa-f]+)',
        content
    ):
        name = m.group(1)
        results['regions'][name] = {
            'exec_base': int(m.group(2), 16),
            'load_base': int(m.group(3), 16),
            'size':      int(m.group(4), 16),
            'max':       int(m.group(5), 16),
        }

    # --- Image component sizes ---
    section = re.search(
        r'Image component sizes\s*(.*?)\s*Grand Totals',
        content, re.DOTALL
    )
    if section:
        for m in re.finditer(
            r'^\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\S+\.o)',
            section.group(1), re.MULTILINE
        ):
            results['modules'].append({
                'code':    int(m.group(1)),
                'inc_data':int(m.group(2)),
                'ro_data': int(m.group(3)),
                'rw_data': int(m.group(4)),
                'zi_data': int(m.group(5)),
                'debug':   int(m.group(6)),
                'object':  m.group(7),
            })

    # --- Grand Totals ---
    m = re.search(
        r'Grand Totals\s*\n\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)',
        content
    )
    if m:
        results['grand_totals'] = {
            'code':    int(m.group(1)),
            'inc_data':int(m.group(2)),
            'ro_data': int(m.group(3)),
            'rw_data': int(m.group(4)),
            'zi_data': int(m.group(5)),
            'debug':   int(m.group(6)),
        }

    # --- Total lines ---
    for m in re.finditer(
        r'Total (RO|RW|ROM)\s+Size[^=]*?=\s*(\d+)', content
    ):
        key = m.group(1).lower() + '_size'
        results['grand_totals'][key] = int(m.group(2))

    return results


def print_map_report(data):
    """打印 map 分析报告"""
    print("=" * 72)
    print("  MAP 文件 RAM 分析报告")
    print("=" * 72)

    # 1) 执行区域
    print("\n--- 执行区域 (Execution Regions) ---\n")
    print(f"{'区域':<16} {'已用':>10} {'上限':>10} {'剩余':>10} {'使用率':>8}")
    print("-" * 58)
    total_used = 0
    total_max  = 0
    for name, info in data['regions'].items():
        used = info['size']
        mx   = info['max']
        free = mx - used
        pct  = used / mx * 100 if mx else 0
        total_used += used
        total_max  += mx
        print(f"{name:<16} {used:>10,} {mx:>10,} {free:>10,} {pct:>7.1f}%")
    free_all = total_max - total_used
    pct_all  = total_used / total_max * 100 if total_max else 0
    print("-" * 58)
    print(f"{'合计':<16} {total_used:>10,} {total_max:>10,} {free_all:>10,} {pct_all:>7.1f}%")

    # 2) 模块 RAM 排序
    print("\n--- 模块 RAM 占用排序 (RW + ZI) ---\n")
    modules = sorted(data['modules'], key=lambda m: m['rw_data'] + m['zi_data'], reverse=True)
    print(f"{'模块':<36} {'RW':>8} {'ZI':>8} {'合计':>8} {'占比':>7}")
    print("-" * 72)
    gt = data['grand_totals']
    total_rw = gt.get('rw_data', 0)
    total_zi = gt.get('zi_data', 0)
    total_ram = total_rw + total_zi
    for m in modules:
        ram = m['rw_data'] + m['zi_data']
        if ram == 0:
            continue
        pct = ram / total_ram * 100 if total_ram else 0
        print(f"{m['object']:<36} {m['rw_data']:>8,} {m['zi_data']:>8,} {ram:>8,} {pct:>6.1f}%")
    print("-" * 72)
    print(f"{'合计':<36} {total_rw:>8,} {total_zi:>8,} {total_ram:>8,} {'100%':>7}")

    # 3) 汇总
    print("\n--- 全局汇总 ---\n")
    for key in ['ro_size', 'rw_size', 'rom_size']:
        if key in gt:
            label = {'ro_size': 'Total RO', 'rw_size': 'Total RW (RAM)', 'rom_size': 'Total ROM (Flash)'}[key]
            val = gt[key]
            print(f"  {label:<24} {val:>10,} B  ({val/1024:.2f} KB)")


# ============================================================
# 模式 2：扫描源文件
# ============================================================
# 匹配全局/静态变量声明（简化版，覆盖常见写法）
RE_STATIC_VAR = re.compile(
    r'^\s*(static\s+)?'                            # static (可选)
    r'(const\s+)?'                                 # const (可选)
    r'([\w\s\*]+?)\s+'                             # 类型 (贪婪最少匹配)
    r'(\w+)'                                       # 变量名
    r'((?:\[\d+\])*)'                              # 可选数组维度
    r'\s*(?:=\s*[^;]+)?;'                          # 可选初始化
)

RE_TYPEDEF_STRUCT = re.compile(
    r'typedef\s+struct\s*\{([^}]*)\}\s*(\w+_t)\s*;',
    re.DOTALL
)


def scan_sources(src_dir):
    """扫描源码目录，估算 RAM 占用"""
    vars_list = []   # (file, name, type_str, array_dims, size)
    struct_defs = {}  # typedef_name -> estimated size

    # 第一遍：收集 struct typedef 大小
    for root, _, files in os.walk(src_dir):
        for fn in files:
            if not fn.endswith(('.c', '.h')):
                continue
            fpath = os.path.join(root, fn)
            with open(fpath, 'r', encoding='utf-8', errors='replace') as f:
                content = f.read()
            # 简单移除注释
            content = re.sub(r'//.*', '', content)
            content = re.sub(r'/\*.*?\*/', '', content, flags=re.DOTALL)

            for m in RE_TYPEDEF_STRUCT.finditer(content):
                tname = m.group(2)
                body = m.group(1)
                # 统计成员
                size = 0
                for line in body.split(';'):
                    line = line.strip()
                    if not line:
                        continue
                    # 提取成员类型
                    parts = line.split()
                    if len(parts) >= 2:
                        member_type = ' '.join(parts[:-1])
                        # 去掉变量名中的数组维度
                        member_name = parts[-1]
                        ms = get_type_size(member_type)
                        # 成员数组
                        dims = RE_ARRAY_DIM.findall(member_name)
                        for d in dims:
                            ms *= int(d)
                        size += ms
                if size > 0:
                    struct_defs[tname] = size
                    TYPE_SIZES[tname] = size

    # 第二遍：扫描全局/静态变量
    for root, _, files in os.walk(src_dir):
        for fn in files:
            if not fn.endswith('.c'):
                continue
            fpath = os.path.join(root, fn)
            rel = os.path.relpath(fpath, src_dir)
            with open(fpath, 'r', encoding='utf-8', errors='replace') as f:
                lines = f.readlines()

            in_function = False
            brace_depth = 0

            for lineno, raw_line in enumerate(lines, 1):
                line = raw_line.strip()
                # 跳过注释
                if line.startswith('//') or line.startswith('/*') or line.startswith('*'):
                    continue

                # 粗略追踪大括号深度，忽略函数体内的局部变量
                brace_depth += line.count('{') - line.count('}')
                if '{' in line and '(' in line and not line.strip().startswith('static'):
                    # 疑似函数定义
                    in_function = True
                if brace_depth <= 0:
                    in_function = False

                # 只关注顶层（全局/文件级 static）变量
                if in_function and brace_depth > 0:
                    # 但 static 局部变量也占 RAM，保留
                    if 'static' not in line:
                        continue

                m = RE_STATIC_VAR.match(line)
                if not m:
                    continue
                is_static = m.group(1) is not None
                is_const = m.group(2) is not None
                type_str = m.group(3).strip().replace('  ', ' ')
                var_name = m.group(4)
                array_part = m.group(5)

                # const 且有初始化 → 可能放 .rodata（Flash），不占 RAM
                if is_const and '=' in line:
                    continue
                # extern → 声明不占空间
                if line.startswith('extern '):
                    continue

                type_size = get_type_size(type_str)
                total_size = calc_var_size(array_part, type_size)

                vars_list.append((rel, lineno, var_name, type_str, array_part or '-', total_size))

    return vars_list, struct_defs


def print_src_report(vars_list, struct_defs):
    """打印源码扫描报告"""
    print("=" * 80)
    print("  源码 RAM 估算报告（静态/全局变量）")
    print("=" * 80)

    # 按 size 降序
    vars_list.sort(key=lambda v: v[5], reverse=True)

    print(f"\n{'文件':<40} {'行':>4} {'变量名':<24} {'类型':<20} {'维度':<8} {'大小':>8}")
    print("-" * 110)

    total = 0
    by_file = defaultdict(int)
    for (fpath, lineno, vname, tstr, dims, size) in vars_list:
        total += size
        by_file[fpath] += size
        print(f"{fpath:<40} {lineno:>4} {vname:<24} {tstr:<20} {dims:<8} {size:>8,}")
    print("-" * 110)
    print(f"{'合计':<96} {total:>8,} B  ({total/1024:.2f} KB)")

    # 按文件汇总
    print(f"\n--- 按文件汇总 ---\n")
    print(f"{'文件':<50} {'RAM':>8}")
    print("-" * 62)
    for fpath, size in sorted(by_file.items(), key=lambda x: x[1], reverse=True):
        print(f"{fpath:<50} {size:>8,}")
    print("-" * 62)
    print(f"{'合计':<50} {total:>8,}")

    # struct 定义
    if struct_defs:
        print(f"\n--- 已识别 struct/typedef 大小 ---\n")
        for name, size in sorted(struct_defs.items(), key=lambda x: x[1], reverse=True):
            print(f"  {name:<36} {size:>6} B")


# ============================================================
# Main
# ============================================================
def main():
    parser = argparse.ArgumentParser(description='STM32 RAM 使用分析')
    parser.add_argument('--map', metavar='FILE', help='Keil .map 文件路径')
    parser.add_argument('--src', metavar='DIR',  help='源码目录路径')
    args = parser.parse_args()

    if not args.map and not args.src:
        parser.print_help()
        sys.exit(1)

    if args.map:
        data = parse_map(args.map)
        print_map_report(data)

    if args.src:
        vars_list, struct_defs = scan_sources(args.src)
        print_src_report(vars_list, struct_defs)


if __name__ == '__main__':
    main()
