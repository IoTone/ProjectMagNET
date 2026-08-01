// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
//
// R25 Stage 2 — build-time packer for the GeoNames cities15000
// dataset. Reads the raw tab-separated `cities15000.txt` (one row
// per city, schema documented at https://download.geonames.org/
// export/dump/readme.txt) and writes a compact binary blob to the
// `assets/data/cities15000.bin` Flutter asset.
//
// Run once on each GeoNames refresh, commit the resulting .bin.
//
//   dart run tool/pack_cities.dart \
//       --input /tmp/cities15000.txt \
//       --output assets/data/cities15000.bin
//
// Pack layout (little-endian throughout):
//   header:
//     4 bytes  magic "CT15"
//     4 bytes  record count (uint32)
//   per-record (var length):
//     4 bytes  latitude  i32  (degrees × 1e6)
//     4 bytes  longitude i32  (degrees × 1e6)
//     4 bytes  population u32 (clamped to 0xFFFFFFFF)
//     2 bytes  country code, ASCII (e.g. "US"; "  " when missing)
//     1 byte   name length N (UTF-8 bytes, max 255 — longer names
//              truncated; rare in cities15000)
//     N bytes  UTF-8 city name (asciiname column, not Unicode name
//              — keeps the asset friendly to a default font)

import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

void main(List<String> argv) {
  String? inputPath;
  String? outputPath;
  for (int i = 0; i < argv.length - 1; i++) {
    if (argv[i] == '--input') inputPath = argv[i + 1];
    if (argv[i] == '--output') outputPath = argv[i + 1];
  }
  inputPath ??= '/tmp/cities15000.txt';
  outputPath ??= 'assets/data/cities15000.bin';
  final File input = File(inputPath);
  if (!input.existsSync()) {
    stderr.writeln('pack_cities: input file not found: $inputPath');
    exit(2);
  }
  final List<String> lines = input.readAsLinesSync();
  stdout.writeln('pack_cities: read ${lines.length} rows from $inputPath');

  final BytesBuilder body = BytesBuilder(copy: false);
  int count = 0;
  int truncatedNames = 0;
  for (final String raw in lines) {
    if (raw.isEmpty) continue;
    final List<String> cols = raw.split('\t');
    if (cols.length < 15) continue;
    // Use the ASCII name column (cols[2]); city names in cities15000
    // are still expressive enough for cell labels without dragging
    // in a Unicode font for Cyrillic / Arabic / Han glyphs.
    final String name = cols[2].trim();
    if (name.isEmpty) continue;
    final double? lat = double.tryParse(cols[4]);
    final double? lon = double.tryParse(cols[5]);
    if (lat == null || lon == null) continue;
    final String country = (cols[8]).padRight(2).substring(0, 2);
    final int population =
        (int.tryParse(cols[14]) ?? 0).clamp(0, 0xFFFFFFFF);

    Uint8List nameBytes = Uint8List.fromList(utf8.encode(name));
    if (nameBytes.length > 255) {
      truncatedNames++;
      // Truncate on a safe UTF-8 boundary: walk backwards until we
      // land on a leading byte (top bit clear, or 11xxxxxx).
      int n = 255;
      while (n > 0 && (nameBytes[n] & 0xC0) == 0x80) {
        n--;
      }
      nameBytes = Uint8List.sublistView(nameBytes, 0, n);
    }

    final ByteData rec = ByteData(15);
    rec.setInt32(0, (lat * 1e6).round(), Endian.little);
    rec.setInt32(4, (lon * 1e6).round(), Endian.little);
    rec.setUint32(8, population, Endian.little);
    rec.setUint8(12, country.codeUnitAt(0));
    rec.setUint8(13, country.codeUnitAt(1));
    rec.setUint8(14, nameBytes.length);
    body.add(rec.buffer.asUint8List());
    body.add(nameBytes);
    count++;
  }

  // Compose final blob.
  final ByteData header = ByteData(8)
    ..setUint8(0, 0x43) // 'C'
    ..setUint8(1, 0x54) // 'T'
    ..setUint8(2, 0x31) // '1'
    ..setUint8(3, 0x35) // '5'
    ..setUint32(4, count, Endian.little);
  final Uint8List bodyBytes = body.toBytes();
  final BytesBuilder out = BytesBuilder(copy: false)
    ..add(header.buffer.asUint8List())
    ..add(bodyBytes);
  final File output = File(outputPath);
  output.parent.createSync(recursive: true);
  output.writeAsBytesSync(out.toBytes());
  stdout.writeln('pack_cities: wrote $count records, '
      '${out.length} bytes → $outputPath');
  if (truncatedNames > 0) {
    stdout.writeln('pack_cities: truncated $truncatedNames long names');
  }
}
