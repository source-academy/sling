import { serialise, SerialiserEntry } from './serialiser';

function flip<T extends string>(o: Record<T, number>): Record<number, T | undefined> {
  // TODO fixme
  // eslint-disable-next-line @typescript-eslint/no-unsafe-return
  return Object.fromEntries(Object.entries(o).map(([k, v]) => [v, k]));
}

export const enum SlingMessageType {
  RUN = 'run',
  STOP = 'stop',
  PING = 'ping',
  STATUS = 'status',
  DISPLAY = 'display',
  INPUT = 'input',
  HELLO = 'hello',
  MONITOR = 'monitor'
}

export const slingDeviceMessageTypes = [
  SlingMessageType.DISPLAY,
  SlingMessageType.STATUS,
  SlingMessageType.HELLO,
  SlingMessageType.MONITOR
];
export const slingClientMessageTypes = [
  SlingMessageType.RUN,
  SlingMessageType.STOP,
  SlingMessageType.PING,
  SlingMessageType.INPUT
];
export const slingMessageTypes = [...slingDeviceMessageTypes, ...slingClientMessageTypes];

const validSlingMessageTypeMap = Object.fromEntries(slingMessageTypes.map((type) => [type, true]));

export function isValidSlingMessageType(type: string): type is SlingMessageType {
  return !!validSlingMessageTypeMap[type];
}

export interface SlingEmptyMessage<T extends SlingMessageType> {
  type: T;
}

export type SlingDisplayPayloadType = keyof typeof displayPayloadTypeToId;

const displayPayloadTypeToId = {
  undefined: 1,
  null: 2,
  boolean: 3,
  i32: 4,
  f32: 5,
  str: 6,
  array: 7,
  function: 8
} as const;

const displayPayloadTypeById = flip<SlingDisplayPayloadType>(displayPayloadTypeToId);

export type SlingDisplayMessageType = keyof typeof displayMessageTypeToId;

const displayMessageTypeToId = {
  output: 0,
  error: 1,
  result: 2,
  response: 4,
  flush: 100
} as const;

const displayMessageTypeById = flip<SlingDisplayMessageType>(displayMessageTypeToId);

export interface SlingEmptyDisplayMessageGeneric<
  T extends SlingMessageType.DISPLAY | SlingMessageType.INPUT,
  MT extends SlingDisplayMessageType
> extends SlingEmptyMessage<T> {
  displayType: MT;
  selfFlushing: boolean;
}

export interface SlingValuedDisplayMessageGeneric<
  T extends SlingMessageType.DISPLAY | SlingMessageType.INPUT,
  PT extends SlingDisplayPayloadType,
  P,
  MT extends SlingDisplayMessageType
> extends SlingEmptyDisplayMessageGeneric<T, MT> {
  payloadType: PT;
  value: P;
}

export type SlingDisplayMessageGeneric<
  T extends SlingMessageType.DISPLAY | SlingMessageType.INPUT,
  MT extends SlingDisplayMessageType
> =
  | SlingValuedDisplayMessageGeneric<T, 'undefined' | 'function', undefined, MT>
  | SlingValuedDisplayMessageGeneric<T, 'null', null, MT>
  | SlingValuedDisplayMessageGeneric<T, 'boolean', boolean, MT>
  | SlingValuedDisplayMessageGeneric<T, 'i32' | 'f32', number, MT>
  | SlingValuedDisplayMessageGeneric<T, 'str' | 'array', string, MT>;

export interface SlingDisplayFlushMessage
  extends SlingEmptyDisplayMessageGeneric<SlingMessageType.DISPLAY, 'flush'> {
  startingId: number;
  endingId: number;
}

export type SlingNonFlushDisplayMessage =
  | SlingDisplayMessageGeneric<SlingMessageType.DISPLAY, 'output' | 'error' | 'result'>
  | SlingDisplayMessageGeneric<SlingMessageType.INPUT, 'response'>;

export type SlingDisplayMessage = SlingNonFlushDisplayMessage | SlingDisplayFlushMessage;

export interface SlingMonitorNonFlushMessage extends SlingEmptyMessage<SlingMessageType.MONITOR> {
  isFlush: boolean;
  data: string;
}

export interface SlingMonitorFlushMessage extends SlingEmptyMessage<SlingMessageType.MONITOR> {
  isFlush: boolean;
  startingId: number;
  endingId: number;
}

export type SlingMonitorMessage = SlingMonitorFlushMessage | SlingMonitorNonFlushMessage;

export type SlingStatus = keyof typeof slingStatusToId;

const slingStatusToId = {
  idle: 0,
  running: 1,
  prompt: 2
} as const;

const slingStatusById = flip<SlingStatus>(slingStatusToId);

export type SlingStatusMessage = SlingEmptyMessage<SlingMessageType.STATUS> &
  ({ status: Exclude<SlingStatus, 'prompt'> } | { status: 'prompt'; prompt: string });

export interface SlingRunMessage extends SlingEmptyMessage<SlingMessageType.RUN> {
  code: Buffer;
}

export type SlingStopMessage = SlingEmptyMessage<SlingMessageType.STOP>;
export type SlingPingMessage = SlingEmptyMessage<SlingMessageType.PING>;
export type SlingHelloMessage = SlingEmptyMessage<SlingMessageType.HELLO> & { nonce: number };

export type SlingNoIdMessage =
  | SlingRunMessage
  | SlingDisplayMessage
  | SlingStatusMessage
  | SlingStopMessage
  | SlingPingMessage
  | SlingHelloMessage
  | SlingMonitorMessage;

export type SlingOptionalIdMessage = SlingNoIdMessage & { id?: number };
export type SlingMessage = SlingNoIdMessage & { id: number };

function parseDisplayPayload(data: Buffer) {
  const payloadType = displayPayloadTypeById[data.readUInt16LE(6)];
  if (!payloadType) {
    return null;
  }

  switch (payloadType) {
    case 'undefined':
    case 'function':
      return { payloadType, value: undefined };
    case 'boolean':
      return { payloadType, value: !!data.readUInt8(8) };
    case 'i32':
      return { payloadType, value: data.readInt32LE(8) };
    case 'f32':
      return { payloadType, value: data.readFloatLE(8) };
    case 'str':
    case 'array': {
      const stringLength = data.readUInt32LE(8);
      const value = data.toString('utf8', 12, 12 + stringLength);
      return { payloadType, value };
    }
  }
  return null;
}

export function deserialiseMqttMessage(topic: string, data: Buffer): SlingMessage | null {
  const splitTopic = topic.split('/');
  if (splitTopic.length < 2) {
    return null;
  }
  const [, type] = splitTopic;
  if (!isValidSlingMessageType(type)) {
    return null;
  }

  const id = data.readUInt32LE(0);

  switch (type) {
    case SlingMessageType.HELLO:
      return { id, type, nonce: data.readUInt32LE(4) };
    case SlingMessageType.PING:
    case SlingMessageType.STOP:
      return { id, type };
    case SlingMessageType.RUN:
      return { id, type, code: data.slice(4) };
    case SlingMessageType.MONITOR: {
      const isFlush = data.readUInt8(4) === 1;
      const common = { id, type, isFlush };
      if (isFlush) {
        return { ...common, startingId: data.readUInt32LE(5), endingId: id };
      }
      return { ...common, data: data.slice(5).toString() };
    }
    case SlingMessageType.STATUS: {
      const status = slingStatusById[data.readUInt16LE(4)];
      if (!status) {
        return null;
      }
      switch (status) {
        case 'prompt': {
          const stringLength = data.readUInt32LE(6);
          return {
            id,
            type,
            status,
            prompt: data.toString('utf8', 10, 10 + stringLength)
          };
        }
        default:
          return { id, type, status };
      }
    }
    case SlingMessageType.DISPLAY: {
      const displayTypeId = data.readUInt16LE(4);
      const displayType = displayMessageTypeById[displayTypeId & 0xff];
      if (!displayType) {
        return null;
      }
      if (displayType === 'flush') {
        return {
          id,
          type,
          displayType,
          startingId: data.readUInt32LE(6),
          endingId: id,
          selfFlushing: false
        };
      } else if (displayType !== 'response') {
        const payload = parseDisplayPayload(data);
        return (
          payload && {
            id,
            type,
            displayType,
            selfFlushing: (displayTypeId & 0xff00) === 0x100,
            ...payload
          }
        );
      }
      return null;
    }
    case SlingMessageType.INPUT: {
      const displayType = displayMessageTypeById[data.readUInt16LE(4)];
      if (displayType === 'response') {
        const payload = parseDisplayPayload(data);
        return (
          payload && {
            id,
            type,
            displayType,
            selfFlushing: false,
            ...payload
          }
        );
      }
    }
  }
  return null;
}

function makeNonce() {
  return Math.floor(Math.random() * 4294967296);
}

export function serialiseMqttMessage(message: SlingOptionalIdMessage): Buffer | null {
  const entries: SerialiserEntry[] = [['u32', message.id || makeNonce()]];
  switch (message.type) {
    case SlingMessageType.PING:
    case SlingMessageType.STOP:
      break;

    case SlingMessageType.HELLO:
      entries.push(['u32', message.nonce]);
      break;

    case SlingMessageType.RUN:
      entries.push(['blob', message.code]);
      break;

    case SlingMessageType.STATUS:
      entries.push(['u16', slingStatusToId[message.status]]);
      if (message.status === 'prompt') {
        entries.push(['str', message.prompt]);
      }
      break;

    case SlingMessageType.INPUT:
    case SlingMessageType.DISPLAY:
      entries.push(['u16', displayMessageTypeToId[message.displayType]]);
      if (message.displayType === 'flush') {
        entries.push(['u32', message.startingId]);
      } else {
        entries.push(['u16', displayPayloadTypeToId[message.payloadType]]);
        switch (message.payloadType) {
          case 'undefined':
          case 'null':
          case 'function':
            break;
          // the following 4 cases are duplicated because typescript isn't
          // powerful enough to tell it is correct if the cases are together
          case 'boolean':
            entries.push([message.payloadType, message.value]);
            break;
          case 'i32':
            entries.push([message.payloadType, message.value]);
            break;
          case 'f32':
            entries.push([message.payloadType, message.value]);
            break;
          case 'str':
          case 'array':
            entries.push(['str', message.value]);
            break;
          default:
            return null;
        }
      }
      break;
    default:
      return null;
  }
  return serialise(entries);
}
