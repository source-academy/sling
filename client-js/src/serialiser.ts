export type SerialiserEntry = InternalSerialiserEntry | ['str', string];

type InternalSerialiserEntry =
  | ['u16' | 'u32' | 'i32' | 'f32', number]
  | ['blob', Buffer]
  | ['boolean', boolean];

const dataTypeSizes = {
  u16: 2,
  u32: 4,
  i32: 4,
  f32: 4,
  boolean: 1
};

export function serialise(entries: SerialiserEntry[]): Buffer {
  let finalSize = 0;
  const processedEntries: InternalSerialiserEntry[] = [];
  for (const entry of entries) {
    if (entry[0] === 'str') {
      const buf = Buffer.from(entry[1], 'utf8');
      finalSize += buf.byteLength + 1;
      processedEntries.push(['blob', buf]);
    } else if (entry[0] === 'blob') {
      finalSize += entry[1].byteLength;
      processedEntries.push(entry);
    } else {
      finalSize += dataTypeSizes[entry[0]];
      processedEntries.push(entry);
    }
  }

  const out = Buffer.alloc(finalSize);
  let position = 0;
  for (const entry of processedEntries) {
    switch (entry[0]) {
      case 'blob':
        position += entry[1].copy(out, position);
        continue;
      case 'u16':
        out.writeUInt16LE(entry[1], position);
        break;
      case 'u32':
        out.writeUInt32LE(entry[1], position);
        break;
      case 'i32':
        out.writeInt32LE(entry[1], position);
        break;
      case 'f32':
        out.writeFloatLE(entry[1], position);
        break;
    }
    position += dataTypeSizes[entry[0]];
  }

  return out;
}
