#!/usr/bin/env node

/**
 * Remote Apple IPSW Component Extractor (via HTTP Range Requests)
 * Allows downloading isolated components (kernelcache, BuildManifest)
 * from 5GB+ Apple IPSW packages without downloading the entire file.
 */

import https from 'https';
import fs from 'fs';
import path from 'path';
import zlib from 'zlib';

function fetchRange(url, start, end) {
  return new Promise((resolve, reject) => {
    const headers = { 'Range': `bytes=${start}-${end}` };
    const req = https.get(url, { headers }, (res) => {
      if (res.statusCode !== 206 && res.statusCode !== 200) {
        return reject(new Error(`HTTP ${res.statusCode} from CDN`));
      }
      const chunks = [];
      res.on('data', chunk => chunks.push(chunk));
      res.on('end', () => resolve(Buffer.concat(chunks)));
      res.on('error', reject);
    });
    req.on('error', reject);
  });
}

function getFileSize(url) {
  return new Promise((resolve, reject) => {
    const req = https.request(url, { method: 'HEAD' }, (res) => {
      const len = res.headers['content-length'];
      if (!len) reject(new Error('No content-length returned from CDN'));
      else resolve(BigInt(len));
    });
    req.on('error', reject);
    req.end();
  });
}

export async function extractRemoteFile(ipswUrl, targetFileName, outputPath) {
  console.log(`\n============================================================`);
  console.log(`  DARWIN-FIRE REMOTE IPSW STREAMING EXTRACTOR`);
  console.log(`  Target: ${targetFileName}`);
  console.log(`============================================================\n`);

  console.log(`[*] Connecting to Apple CDN: ${ipswUrl}`);
  const totalSize = await getFileSize(ipswUrl);
  console.log(`[+] Total IPSW Size: ${(Number(totalSize) / 1024 / 1024).toFixed(2)} MB (${totalSize} bytes)`);

  // Read last 128KB of the file to find ZIP64 / EOCD
  const tailSize = 128 * 1024;
  const tailStart = totalSize - BigInt(tailSize);
  console.log(`[*] Fetching ZIP trailer (Range: ${tailStart} - ${totalSize - 1n})...`);
  const tailBuf = await fetchRange(ipswUrl, tailStart, totalSize - 1n);

  // Search for ZIP64 EOCD Locator: magic 0x07064b50
  let zip64LocOffset = -1;
  for (let i = tailBuf.length - 20; i >= 0; i--) {
    if (tailBuf.readUInt32LE(i) === 0x07064b50) {
      zip64LocOffset = i;
      break;
    }
  }

  let cdOffset = 0n;
  let cdSize = 0n;

  if (zip64LocOffset !== -1) {
    console.log(`[+] Found ZIP64 Locator at relative offset ${zip64LocOffset}`);
    const zip64EOCDOffset = tailBuf.readBigUInt64LE(zip64LocOffset + 8);
    console.log(`[+] ZIP64 EOCD Record located at byte offset: ${zip64EOCDOffset}`);

    // Fetch ZIP64 EOCD record (56 bytes)
    const eocd64Buf = await fetchRange(ipswUrl, zip64EOCDOffset, zip64EOCDOffset + 56n);
    if (eocd64Buf.readUInt32LE(0) === 0x06064b50) {
      cdSize = eocd64Buf.readBigUInt64LE(40);
      cdOffset = eocd64Buf.readBigUInt64LE(48);
      console.log(`[+] Central Directory offset: ${cdOffset} | size: ${cdSize} bytes`);
    } else {
      throw new Error('Invalid ZIP64 EOCD magic');
    }
  } else {
    // Standard ZIP EOCD: magic 0x06054b50
    let eocdOffset = -1;
    for (let i = tailBuf.length - 22; i >= 0; i--) {
      if (tailBuf.readUInt32LE(i) === 0x06054b50) {
        eocdOffset = i;
        break;
      }
    }
    if (eocdOffset === -1) throw new Error('Could not find ZIP End of Central Directory');
    cdSize = BigInt(tailBuf.readUInt32LE(eocdOffset + 12));
    cdOffset = BigInt(tailBuf.readUInt32LE(eocdOffset + 16));
    console.log(`[+] Standard Central Directory offset: ${cdOffset} | size: ${cdSize} bytes`);
  }

  // Fetch Central Directory
  console.log(`[*] Fetching Central Directory (${(Number(cdSize) / 1024).toFixed(2)} KB)...`);
  const cdBuf = await fetchRange(ipswUrl, cdOffset, cdOffset + cdSize - 1n);

  // Parse entries to find target file
  let off = 0;
  let matchedEntry = null;
  const entries = [];

  while (off + 46 <= cdBuf.length) {
    const sig = cdBuf.readUInt32LE(off);
    if (sig !== 0x02014b50) break;

    const method = cdBuf.readUInt16LE(off + 10);
    let compSize = BigInt(cdBuf.readUInt32LE(off + 20));
    let uncompSize = BigInt(cdBuf.readUInt32LE(off + 24));
    const nameLen = cdBuf.readUInt16LE(off + 28);
    const extraLen = cdBuf.readUInt16LE(off + 30);
    const commentLen = cdBuf.readUInt16LE(off + 32);
    let localHeaderOffset = BigInt(cdBuf.readUInt32LE(off + 42));

    const filename = cdBuf.toString('ascii', off + 46, off + 46 + nameLen);

    // Parse ZIP64 extra field if needed (header ID 0x0001)
    let extraOff = off + 46 + nameLen;
    const extraEnd = extraOff + extraLen;
    while (extraOff + 4 <= extraEnd) {
      const headerId = cdBuf.readUInt16LE(extraOff);
      const dataSize = cdBuf.readUInt16LE(extraOff + 2);
      if (headerId === 0x0001) {
        let fieldPos = extraOff + 4;
        if (uncompSize === 0xffffffffn && fieldPos + 8 <= extraEnd) {
          uncompSize = cdBuf.readBigUInt64LE(fieldPos); fieldPos += 8;
        }
        if (compSize === 0xffffffffn && fieldPos + 8 <= extraEnd) {
          compSize = cdBuf.readBigUInt64LE(fieldPos); fieldPos += 8;
        }
        if (localHeaderOffset === 0xffffffffn && fieldPos + 8 <= extraEnd) {
          localHeaderOffset = cdBuf.readBigUInt64LE(fieldPos); fieldPos += 8;
        }
      }
      extraOff += 4 + dataSize;
    }

    entries.push(filename);
    if (filename.toLowerCase().includes(targetFileName.toLowerCase())) {
      matchedEntry = {
        filename,
        method,
        compSize,
        uncompSize,
        localHeaderOffset
      };
      break;
    }

    off += 46 + nameLen + extraLen + commentLen;
  }

  if (!matchedEntry) {
    console.error(`[!] Target file "${targetFileName}" not found in Central Directory.`);
    console.log(`[+] Available top files:`);
    entries.slice(0, 15).forEach(e => console.log(`    - ${e}`));
    return;
  }

  console.log(`\n[+] MATCH FOUND: ${matchedEntry.filename}`);
  console.log(`    Compressed Size:   ${(Number(matchedEntry.compSize) / 1024 / 1024).toFixed(2)} MB (${matchedEntry.compSize} bytes)`);
  console.log(`    Uncompressed Size: ${(Number(matchedEntry.uncompSize) / 1024 / 1024).toFixed(2)} MB (${matchedEntry.uncompSize} bytes)`);
  console.log(`    Local Header At:   ${matchedEntry.localHeaderOffset}`);
  console.log(`    Compression Method: ${matchedEntry.method === 0 ? 'Store (Uncompressed)' : matchedEntry.method === 8 ? 'Deflate' : matchedEntry.method}`);

  // Fetch local file header (30 bytes + name + extra)
  const localHeaderBuf = await fetchRange(ipswUrl, matchedEntry.localHeaderOffset, matchedEntry.localHeaderOffset + 128n);
  const localNameLen = localHeaderBuf.readUInt16LE(26);
  const localExtraLen = localHeaderBuf.readUInt16LE(28);
  const dataStart = matchedEntry.localHeaderOffset + 30n + BigInt(localNameLen + localExtraLen);
  const dataEnd = dataStart + matchedEntry.compSize - 1n;

  console.log(`[*] Downloading payload bytes (Range: ${dataStart} - ${dataEnd})...`);
  const rawData = await fetchRange(ipswUrl, dataStart, dataEnd);

  let finalData;
  if (matchedEntry.method === 0) {
    finalData = rawData;
  } else if (matchedEntry.method === 8) {
    console.log(`[*] Inflating Deflate data...`);
    finalData = zlib.inflateRawSync(rawData);
  } else {
    throw new Error(`Unsupported compression method: ${matchedEntry.method}`);
  }

  const outDir = path.dirname(outputPath);
  if (!fs.existsSync(outDir)) fs.mkdirSync(outDir, { recursive: true });
  fs.writeFileSync(outputPath, finalData);

  console.log(`\n============================================================`);
  console.log(`[SUCCESS] Extracted and saved to: ${path.resolve(outputPath)}`);
  console.log(`File Size: ${(finalData.length / 1024 / 1024).toFixed(2)} MB (${finalData.length} bytes)`);
  console.log(`============================================================\n`);
}

// Test / CLI execution
const target = process.argv[2] || 'kernelcache';
const out = process.argv[3] || 'extracted_binaries/kernelcache.release';
const defaultUrl = 'https://updates.cdn-apple.com/2025FallFCS/fullrestores/122-77496/774C7319-F63C-4C05-9C6C-49F5F0966540/iPhone_4.7_P3_15.8.8_19H422_Restore.ipsw';

extractRemoteFile(defaultUrl, target, out).catch(err => {
  console.error(`[!] Extractor error:`, err.message);
});
