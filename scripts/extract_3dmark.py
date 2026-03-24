#!/usr/bin/env python3
"""Extract scores from all .3dmark-result ZIP files into a single JSON."""

import zipfile
import xml.etree.ElementTree as ET
import os
import json
import glob

RESULTS_DIR = os.path.join(os.path.dirname(__file__), '..', '3DMark Results')

results = {}

for path in sorted(glob.glob(os.path.join(RESULTS_DIR, '**', '*.3dmark-result'), recursive=True)):
    path_norm = path.replace('\\', '/')
    parts = path_norm.split('/')
    # Find the GPU directory name (first dir under "3DMark Results")
    try:
        idx = next(i for i, p in enumerate(parts) if p == '3DMark Results')
        gpu_dir = parts[idx + 1]
    except StopIteration:
        continue

    fname = parts[-1]

    try:
        with zipfile.ZipFile(path) as z:
            xml_data = z.read('Result.xml').decode('utf-8')
        root = ET.fromstring(xml_data)

        for result_elem in root.findall('.//result'):
            pass_idx = result_elem.find('passIndex')
            if pass_idx is not None and pass_idx.text == '-1':
                scores = {}
                for child in result_elem:
                    tag = child.tag
                    if tag.lower() in ('name', 'description', 'passindex', 'benchmarkrunid'):
                        continue
                    try:
                        scores[tag] = float(child.text) if '.' in str(child.text) else int(child.text)
                    except (ValueError, TypeError):
                        scores[tag] = child.text

                if gpu_dir not in results:
                    results[gpu_dir] = {}

                # Derive benchmark type from filename
                base = fname.replace('.3dmark-result', '').replace('-result', '')
                # Split and find benchmark code
                bench_codes = ['ts', 'fs', 'sn', 'snl', 'api', 'cg', 'is', 'ise', 'nr', 'wl', 'wle']
                name_parts = base.split('-')
                bench_type = 'unknown'
                for i, p in enumerate(name_parts):
                    if p in bench_codes:
                        bench_type = '-'.join(name_parts[i:])
                        break

                results[gpu_dir][bench_type] = scores
                break
    except Exception as e:
        print(f'ERROR: {path}: {e}')

# Print summary
for gpu in sorted(results.keys()):
    print(f'\n=== {gpu} ===')
    for bench in sorted(results[gpu].keys()):
        scores = results[gpu][bench]
        print(f'  {bench}:')
        for k, v in scores.items():
            print(f'    {k}: {v}')

# Save
out_path = os.path.join(os.path.dirname(__file__), '3dmark_extracted.json')
with open(out_path, 'w') as f:
    json.dump(results, f, indent=2)
print(f'\nSaved to {out_path}')
