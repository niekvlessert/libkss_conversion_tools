#!/usr/bin/env python3
from __future__ import annotations

import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zipfile

ZX0_JAR = Path('/mnt/data/zx0java/zx0.jar')

BATCH_JAR = Path('/mnt/data/zx0java/zx0batch.jar')

def compress_all(streams: list[bytes], tmp: Path) -> list[bytes]:
    args = ['java', '-cp', f'{ZX0_JAR}:{BATCH_JAR}', 'zx0.Batch']
    outputs = []
    for i, raw in enumerate(streams):
        src = tmp / f'b{i:04d}.raw'
        dst = tmp / f'b{i:04d}.zx0'
        src.write_bytes(raw)
        args.extend([str(src), str(dst)])
        outputs.append(dst)
    proc = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    if proc.returncode:
        raise RuntimeError(proc.stdout.decode('utf-8', 'replace'))
    return [p.read_bytes() for p in outputs]

def decompress_all(streams: list[bytes], tmp: Path) -> list[bytes]:
    args = ['java', '-cp', f'{ZX0_JAR}:{BATCH_JAR}', 'zx0.BatchDecompress']
    outputs = []
    for i, packed in enumerate(streams):
        src = tmp / f'u{i:04d}.zx0'
        dst = tmp / f'u{i:04d}.raw'
        src.write_bytes(packed)
        args.extend([str(src), str(dst)])
        outputs.append(dst)
    proc = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    if proc.returncode:
        raise RuntimeError(proc.stdout.decode('utf-8', 'replace'))
    return [p.read_bytes() for p in outputs]

PACKS = [
    {
        'name': 'Parodius Da!',
        'src_dir': Path('/mnt/data/work_kcpz/parodius'),
        'src': 'Parodius_Da_PC_Engine_SCCplus_prototype.ksp',
        'out_dir': Path('/mnt/data/parodius_sccplus_ksp'),
        'out': 'Parodius_Da_PC_Engine_SCCplus_compressed.ksp',
        'bundle': 'Parodius_Da_PC_Engine_SCCplus_compressed_bundle.zip',
    },
    {
        'name': 'Magical Chase',
        'src_dir': Path('/mnt/data/work_kcpz/magical'),
        'src': 'Magical_Chase_PC_Engine_SCCplus_chunked_prototype.ksp',
        'out_dir': Path('/mnt/data/magical_chase_sccplus_ksp'),
        'out': 'Magical_Chase_PC_Engine_SCCplus_chunked_compressed.ksp',
        'bundle': 'Magical_Chase_PC_Engine_SCCplus_chunked_compressed_bundle.zip',
    },
    {
        'name': "Bomberman '94",
        'src_dir': Path('/mnt/data/work_kcpz/bomberman'),
        'src': 'Bomberman_94_PC_Engine_SCCplus_chunked_prototype.ksp',
        'out_dir': Path('/mnt/data/bomberman94_sccplus_ksp'),
        'out': 'Bomberman_94_PC_Engine_SCCplus_chunked_compressed.ksp',
        'bundle': 'Bomberman_94_PC_Engine_SCCplus_chunked_compressed_bundle.zip',
    },
]


def u16(data: bytes, off: int) -> int:
    return struct.unpack_from('<H', data, off)[0]


def parse_kcpx(path: Path) -> dict:
    data = path.read_bytes()
    if data[:4] not in (b'KSCC', b'KSSX'):
        raise ValueError(f'{path}: not KSS/KSP')
    load_size = u16(data, 6)
    kcp_off = 0x20 + load_size
    if data[kcp_off:kcp_off+4] != b'KCPX':
        raise ValueError(f'{path}: expected KCPX at {kcp_off:#x}')
    fixed = bytearray(data[kcp_off:kcp_off+528])
    version = fixed[4]
    page_count = fixed[5]
    track_count = fixed[6]
    if version != 1 or not page_count:
        raise ValueError((version, page_count))
    p = kcp_off + 528
    template = data[p:p+0x4000]
    if len(template) != 0x4000:
        raise ValueError('short template')
    p += 0x4000
    overlays: list[bytes] = []
    for _ in range(page_count):
        n = u16(data, p)
        p += 2
        overlays.append(data[p:p+n])
        p += n
    if p != len(data):
        raise ValueError(f'unparsed bytes: {len(data)-p}')
    return {
        'data': data,
        'prefix': data[:kcp_off],
        'kcp_off': kcp_off,
        'fixed': fixed,
        'page_count': page_count,
        'track_count': track_count,
        'template': template,
        'overlays': overlays,
    }


def compress_one(raw: bytes, tmp: Path, index: int) -> bytes:
    src = tmp / f'{index:04d}.raw'
    dst = tmp / f'{index:04d}.zx0'
    src.write_bytes(raw)
    proc = subprocess.run(
        ['java', '-Xmx512m', '-jar', str(ZX0_JAR), '-p4', '-f', str(src), str(dst)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if proc.returncode:
        raise RuntimeError(proc.stdout.decode('utf-8', 'replace'))
    return dst.read_bytes()


def decompress_one(packed: bytes, tmp: Path, index: int) -> bytes:
    src = tmp / f'd{index:04d}.zx0'
    dst = tmp / f'd{index:04d}.raw'
    src.write_bytes(packed)
    proc = subprocess.run(
        ['java', '-jar', str(ZX0_JAR), '-f', '-d', str(src), str(dst)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if proc.returncode:
        raise RuntimeError(proc.stdout.decode('utf-8', 'replace'))
    return dst.read_bytes()


def materialize(template: bytes, overlay: bytes) -> bytes:
    page = bytearray(template)
    p = 0
    while p < len(overlay):
        if p + 4 > len(overlay):
            raise ValueError('truncated overlay header')
        offset, length = struct.unpack_from('<HH', overlay, p)
        p += 4
        if p + length > len(overlay) or offset + length > 0x4000:
            raise ValueError('bad overlay record')
        page[offset:offset+length] = overlay[p:p+length]
        p += length
    return bytes(page)


def build_kcpz(parsed: dict, packed_template: bytes, packed_overlays: list[bytes]) -> tuple[bytes, int]:
    fixed = bytearray(parsed['fixed'])
    fixed[:4] = b'KCPZ'

    # First calculate offsets using six-byte v1 records; switch to v2 when
    # any absolute KCP-relative packed-stream offset exceeds 16 bits.
    v1_data_start = 528 + 4 + parsed['page_count'] * 6
    cursor = v1_data_start + len(packed_template)
    v1_offsets = []
    for packed in packed_overlays:
        v1_offsets.append(cursor)
        cursor += len(packed)
    version = 2 if any(off > 0xFFFF for off in v1_offsets) else 1
    fixed[4] = version
    record_size = 8 if version == 2 else 6

    tail = bytearray(fixed)
    tail.extend(b'\0' * (4 + parsed['page_count'] * record_size))
    template_offset = len(tail)
    if len(packed_template) > 0xFFFF or template_offset > 0xFFFF:
        raise ValueError('template descriptor overflow')
    struct.pack_into('<HH', tail, 528, len(packed_template), template_offset)
    tail.extend(packed_template)

    for i, (raw, packed) in enumerate(zip(parsed['overlays'], packed_overlays)):
        if len(raw) > 0xFFFF or len(packed) > 0xFFFF:
            raise ValueError(f'overlay {i} size overflow')
        desc = 532 + i * record_size
        struct.pack_into('<HH', tail, desc, len(raw), len(packed))
        stream_offset = len(tail)
        if version == 1:
            if stream_offset > 0xFFFF:
                raise ValueError('v1 offset overflow after selection')
            struct.pack_into('<H', tail, desc + 4, stream_offset)
        else:
            struct.pack_into('<I', tail, desc + 4, stream_offset)
        tail.extend(packed)

    return parsed['prefix'] + bytes(tail), version


def parse_kcpz(data: bytes) -> dict:
    load_size = u16(data, 6)
    off = 0x20 + load_size
    fixed = data[off:off+528]
    if fixed[:4] != b'KCPZ':
        raise ValueError('not KCPZ')
    version = fixed[4]
    count = fixed[5]
    record_size = 8 if version == 2 else 6
    packed_template_size, template_offset = struct.unpack_from('<HH', data, off+528)
    packed_template = data[off+template_offset:off+template_offset+packed_template_size]
    overlays = []
    for i in range(count):
        d = off + 532 + i * record_size
        raw_size, packed_size = struct.unpack_from('<HH', data, d)
        if version == 1:
            stream_off = u16(data, d+4)
        else:
            stream_off = struct.unpack_from('<I', data, d+4)[0]
        packed = data[off+stream_off:off+stream_off+packed_size]
        overlays.append((raw_size, packed))
    return {'off': off, 'version': version, 'count': count, 'packed_template': packed_template, 'overlays': overlays}


def revised_chunk_patch(original: str) -> str:
    # Deliver a stand-alone replacement patch. It deliberately dispatches raw
    # KCPX overlays to the old scanner and KCPZ overlays to an overlay-only
    # ZX0 path, preserving 4000H-47FFH engine/state during an in-track switch.
    return r'''--- a/msx/PLAYER_LAYOUT.inc
+++ b/msx/PLAYER_LAYOUT.inc
@@
 CUSTOM_ENASLT: equ 0xD840
+; Runtime-only KCP overlay switch gateway used by chunked HuC6280/SCC+ packs.
+; A = internal KCP page index. It replaces only the sparse overlay and keeps
+; the common page-1 template, engine and runtime state intact.
+KCP_PAGE_GATEWAY: equ 0xD8A0
@@
--- a/msx/KSPPLAYER.asm
+++ b/msx/KSPPLAYER.asm
@@
 kcpx_runtime_path:
@@
         call    kcpx_materialize_dispatch
         jp      c,format_error
+
+        ; Install the fixed page-3 gateway called by chunk-aware engines.
+        ld      hl,kcp_page_gateway
+        ld      de,KCP_PAGE_GATEWAY
+        ld      bc,kcp_page_gateway_end-kcp_page_gateway
+        ldir
@@
 kcpx_silence:
@@
         ld      a,0x3F
         ld      (0x9000),a
+        ; Silence SCC-I/SCC+ register mode as well.
+        xor     a
+        ld      hl,0xB8AA
+        ld      b,6
+kcp_silence_sccplus:
+        ld      (hl),a
+        inc     hl
+        djnz    kcp_silence_sccplus
         xor     a
         ld      hl,0x9880
@@
+; Replace only the selected sparse overlay. KCPX records are copied directly
+; from staged storage. KCPZ records are ZX0-expanded into the temporary mapper
+; segment and applied through page 2. Neither path recopies the 16K template.
+kcp_materialize_overlay_only:
+        ld      a,(kcp_format)
+        cp      5
+        jr      z,kcpx_materialize_overlay_only
+        cp      6
+        jr      z,kcpz_materialize_overlay_only
+        scf
+        ret
+
+kcpx_materialize_overlay_only:
+        ld      a,(RUNTIME_KSS_TABLE)
+        ld      (kcp_selected_segment),a
+        ld      (kcp_dest_segment),a
+        ld      hl,(kcp_records_source)
+        ld      (kcp_cursor),hl
+        ld      a,(kcp_selected_page)
+        ld      (kcp_page_index),a
+        jp      kcpx_overlay_scan
+
+kcpz_materialize_overlay_only:
+        ld      a,(RUNTIME_KSS_TABLE)
+        ld      (kcp_selected_segment),a
+        ld      a,(RUNTIME_KSS_TABLE+1)
+        ld      (kcp_temp_segment),a
+        call    kcpz_read_selected_descriptor
+
+        ; Decode only the selected overlay into the temporary segment.
+        ld      a,(kcp_temp_segment)
+        ld      (kcp_dest_segment),a
+        call    PUT_P1_DISPATCH
+        ld      hl,(kcp_data_source)
+        call    kcpx_set_source
+        ld      a,(kcp_data_source_high)
+        ld      (source_position+2),a
+        ld      de,0x4000
+        call    kcpz_decompress_source
+
+        ; Apply its sparse records to the already-running page-1 image.
+        ld      a,(kcp_selected_segment)
+        ld      (kcp_dest_segment),a
+        call    PUT_P1_DISPATCH
+        call    kcpz_select_ram_page2
+        ld      a,(kcp_temp_segment)
+        call    PUT_P2_DISPATCH
+        ld      hl,0x8000
+        ld      de,(kcp_data_size)
+        ld      (kcp_remaining),de
+        jp      kcpz_overlay_loop
+
+; This block is copied to KCP_PAGE_GATEWAY (D8A0H). A contains the internal
+; logical page. It is called with interrupts disabled from the page-1 engine.
+kcp_page_gateway:
+        push    af
+        push    bc
+        push    de
+        push    hl
+        push    ix
+        push    iy
+        ld      (kcp_selected_page),a
+        call    kcp_materialize_overlay_only
+        jr      c,kcp_page_gateway_fail
+        ld      a,(kcp_selected_segment)
+        call    PUT_P1_DISPATCH
+        ld      a,(RUNTIME_SCC_SLOT)
+        ld      h,0x80
+        call    CUSTOM_ENASLT
+        ; Restore SCC-I/SCC+ mode after page-2 source mapping.
+        ld      a,0x20
+        ld      (0xBFFE),a
+        ld      a,0x80
+        ld      (0xB000),a
+kcp_page_gateway_fail:
+        pop     iy
+        pop     ix
+        pop     hl
+        pop     de
+        pop     bc
+        pop     af
+        ret
+kcp_page_gateway_end:
+'''


def process(pack: dict) -> dict:
    src = pack['src_dir'] / pack['src']
    parsed = parse_kcpx(src)
    streams = [parsed['template'], *parsed['overlays']]
    with tempfile.TemporaryDirectory(prefix='kcpz_pack_') as td:
        tmp = Path(td)
        packed_streams = [compress_one(raw, tmp, i) for i, raw in enumerate(streams)]
        packed_template = packed_streams[0]
        packed_overlays = packed_streams[1:]
        out_data, version = build_kcpz(parsed, packed_template, packed_overlays)

        # Independent parse/decompress validation of every stream.
        decoded = parse_kcpz(out_data)
        if decoded['version'] != version or decoded['count'] != parsed['page_count']:
            raise AssertionError('descriptor mismatch')
        raw_streams: list[bytes] = []
        dec_inputs = [decoded['packed_template'], *[p for _, p in decoded['overlays']]]
        raw_streams = [decompress_one(raw, tmp, i) for i, raw in enumerate(dec_inputs)]
        if raw_streams[0] != parsed['template']:
            raise AssertionError('template parity failed')
        page_reports = []
        for i, ((expected_overlay), (raw_size, _), actual_overlay) in enumerate(zip(parsed['overlays'], decoded['overlays'], raw_streams[1:])):
            if raw_size != len(expected_overlay) or actual_overlay != expected_overlay:
                raise AssertionError(f'overlay parity failed {i}')
            raw_page = materialize(parsed['template'], expected_overlay)
            cmp_page = materialize(raw_streams[0], actual_overlay)
            if raw_page != cmp_page:
                raise AssertionError(f'page parity failed {i}')
            page_reports.append({
                'page': i,
                'raw_overlay_bytes': len(expected_overlay),
                'packed_overlay_bytes': len(decoded['overlays'][i][1]),
                'page_sha256': hashlib.sha256(raw_page).hexdigest(),
            })

    pack['out_dir'].mkdir(parents=True, exist_ok=True)
    out_path = pack['out_dir'] / pack['out']
    out_path.write_bytes(out_data)
    report = {
        'name': pack['name'],
        'source': src.name,
        'source_bytes': len(parsed['data']),
        'source_sha256': hashlib.sha256(parsed['data']).hexdigest(),
        'output': out_path.name,
        'output_bytes': len(out_data),
        'output_sha256': hashlib.sha256(out_data).hexdigest(),
        'reduction_bytes': len(parsed['data']) - len(out_data),
        'reduction_percent': round((1 - len(out_data) / len(parsed['data'])) * 100, 2),
        'container': 'KCPZ',
        'kcpz_version': version,
        'public_tracks': parsed['track_count'],
        'internal_pages': parsed['page_count'],
        'template_raw_bytes': len(parsed['template']),
        'template_packed_bytes': len(packed_template),
        'all_streams_roundtrip_exact': True,
        'all_materialized_pages_byte_identical_to_kcpx': True,
        'pages': page_reports,
    }
    report_path = pack['out_dir'] / (Path(pack['out']).stem + '_validation.json')
    report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')

    note = f'''# {pack['name']} SCC+ compressed KSP\n\nThis is the ZX0-compressed `KCPZ` counterpart of `{src.name}`.\n\n- KCPZ version: {version}\n- Public tracks: {parsed['track_count']}\n- Internal pages: {parsed['page_count']}\n- Raw KCPX size: {len(parsed['data']):,} bytes\n- Compressed KCPZ size: {len(out_data):,} bytes\n- Reduction: {report['reduction_percent']:.2f}%\n- SHA-256: `{report['output_sha256']}`\n\nThe common 16 KB template and every sparse page overlay are independently compressed with standard forward ZX0 v2. Every stream was decompressed again and compared byte-for-byte with the KCPX source. Every reconstructed 16 KB page is byte-identical to the uncompressed container.\n'''
    if parsed['page_count'] > parsed['track_count']:
        note += '''\n## Chunked-player requirement\n\nThis pack changes internal overlays while a track is playing. Apply `kspplayer_chunked_sccplus_kcpz.patch`; it adds an overlay-only KCPZ path that preserves the resident engine and state while ZX0-expanding the next overlay.\n'''
    else:
        note += '''\nThe normal current KCPZ materializer is sufficient for song changes. The included SCC+ silence patch remains applicable.\n'''
    note_path = pack['out_dir'] / (Path(pack['out']).stem + '_README.md')
    note_path.write_text(note, encoding='utf-8')

    # Build a complete bundle by carrying over the original prototype support
    # files and adding the compressed artifact and exact-parity report.
    staging = pack['out_dir'] / (Path(pack['bundle']).stem + '_staging')
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir()
    for f in pack['src_dir'].iterdir():
        if f.is_file():
            shutil.copy2(f, staging / f.name)
    shutil.copy2(out_path, staging / out_path.name)
    shutil.copy2(report_path, staging / report_path.name)
    shutil.copy2(note_path, staging / note_path.name)
    shutil.copy2(Path(__file__), staging / Path(__file__).name)
    if parsed['page_count'] > parsed['track_count']:
        patch_path = staging / 'kspplayer_chunked_sccplus_kcpz.patch'
        patch_path.write_text(revised_chunk_patch(''), encoding='utf-8')
    bundle_path = pack['out_dir'] / pack['bundle']
    with zipfile.ZipFile(bundle_path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for f in sorted(staging.iterdir()):
            zf.write(f, arcname=f.name)
    shutil.rmtree(staging)
    report['validation_path'] = str(report_path)
    report['bundle_path'] = str(bundle_path)
    return report


def main() -> None:
    if not ZX0_JAR.exists():
        raise SystemExit(f'missing {ZX0_JAR}')
    results = []
    only = os.environ.get('PACK_ONLY', '').lower().strip()
    selected = [p for p in PACKS if not only or only in p['name'].lower()]
    for pack in selected:
        print(f"Compressing {pack['name']}...", flush=True)
        result = process(pack)
        results.append(result)
        print(f"  {result['source_bytes']} -> {result['output_bytes']} bytes ({result['reduction_percent']}%)", flush=True)
    summary = Path('/mnt/data/sccplus_kcpz_conversion_summary.json')
    summary.write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
    print(summary)

if __name__ == '__main__':
    main()
