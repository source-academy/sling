import { TypedEmitter } from 'tiny-typed-emitter';
import { MqttClient, connect as mqttConnect } from 'mqtt';
import {
  slingDeviceMessageTypes,
  deserialiseMqttMessage,
  SlingMessage,
  SlingMessageType,
  serialiseMqttMessage,
  SlingOptionalIdMessage,
  SlingNonFlushDisplayMessage,
  SlingDisplayFlushMessage,
  SlingDisplayMessageType
} from './slingProtocol';

export interface SlingClientOptions {
  /**
   * The WebSocket endpoint URL to connect to.
   */
  readonly websocketEndpoint: string;
  /**
   * The MQTT client ID for this client to use.
   */
  readonly clientId: string;
  /**
   * The MQTT client ID of the device.
   */
  readonly deviceId: string;
}

export interface SlingClientEvents {
  connect: () => void;
  error: (error: Error) => void;
  message: (message: SlingMessage) => void;
  statusChange: (isRunning: boolean) => void;
  prompt: (prompt: string) => void;
  promptDismiss: () => void;
  display: (message: string, type: SlingDisplayMessageType) => void;
}

export class SlingClient extends TypedEmitter<SlingClientEvents> {
  readonly options: SlingClientOptions;

  private _mqttClient?: MqttClient;
  private _deviceStatus?: {
    running: boolean;
    prompt?: string;
  };
  private _lastProcessedMessageId?: number;
  private _queuedMessages = new Map<number, SlingMessage>();

  private readonly _displayBuffer = new Map<number, SlingNonFlushDisplayMessage>();
  private readonly _queuedFlushes = new Set<SlingDisplayFlushMessage>();

  constructor(options: SlingClientOptions) {
    super();
    this.options = options;
  }

  connect(): void {
    this._mqttClient = mqttConnect(this.options.websocketEndpoint, {
      clientId: this.options.clientId
    });
    this._mqttClient.on('connect', () => {
      this._handleConnect();
    });
    this._mqttClient.on('error', (error) => {
      this.emit('error', error);
    });
    this._mqttClient.on('message', (topic, payload) => {
      this._handleMessage(topic, payload);
    });
  }

  disconnect(): void {
    if (!this._mqttClient) {
      return;
    }
    this._mqttClient.removeAllListeners();
    this._mqttClient.end();
    this._deviceStatus = undefined;
  }

  sendRun(code: Buffer): void {
    this.sendMessage({ type: SlingMessageType.RUN, code });
  }

  sendStop(): void {
    this.sendMessage({ type: SlingMessageType.STOP });
  }

  sendPing(): void {
    this.sendMessage({ type: SlingMessageType.PING });
  }

  sendMessage(message: SlingOptionalIdMessage): void {
    if (!this._mqttClient) {
      return;
    }

    const mqttPayload = serialiseMqttMessage(message);
    if (!mqttPayload) {
      return;
    }

    this._mqttClient.publish(`${this.options.deviceId}/${message.type}`, mqttPayload, {
      qos: 1
    });
  }

  private _handleConnect(): void {
    if (!this._mqttClient) {
      return;
    }
    this._mqttClient.subscribe(
      slingDeviceMessageTypes.map((type) => `${this.options.deviceId}/${type}`),
      { qos: 1 }
    );
    this.sendPing();
  }

  private _handleMessage(topic: string, payload: Buffer): void {
    const message = deserialiseMqttMessage(topic, payload);
    if (!message || (this._lastProcessedMessageId || -1) >= message.id) {
      return;
    }

    if (
      this._lastProcessedMessageId === undefined ||
      message.id === this._lastProcessedMessageId + 1 ||
      (this._lastProcessedMessageId >= 4_000_000_000 && message.id === 0)
    ) {
      this._processMessage(message);
    } else {
      this._queuedMessages.set(message.id, message);
    }

    if (!this._lastProcessedMessageId) {
      return;
    }

    let queuedMessage = this._queuedMessages.get(this._lastProcessedMessageId + 1);
    while (queuedMessage) {
      this._queuedMessages.delete(this._lastProcessedMessageId + 1);
      this._processMessage(queuedMessage);
      queuedMessage = this._queuedMessages.get(this._lastProcessedMessageId + 1);
    }
  }

  private _processMessage(message: SlingMessage): void {
    this._lastProcessedMessageId = message.id;
    switch (message.type) {
      case SlingMessageType.STATUS: {
        const oldRunning = this._deviceStatus?.running;
        this._deviceStatus = {
          ...this._deviceStatus,
          running: message.status !== 'idle'
        };
        if (oldRunning !== this._deviceStatus.running) {
          this.emit('statusChange', this._deviceStatus.running);
        }
        if (message.status === 'prompt') {
          this._deviceStatus.prompt = message.prompt;
          this.emit('prompt', message.prompt);
        } else if (this._deviceStatus.prompt) {
          this._deviceStatus.prompt = undefined;
          this.emit('promptDismiss');
        }
        break;
      }

      case SlingMessageType.DISPLAY: {
        if (message.displayType === 'flush') {
          this._queuedFlushes.add(message);
          this._attemptFlush(message);
        } else {
          this._displayBuffer.set(message.id, message);
          for (const flush of this._queuedFlushes) {
            if (flush.startingId <= message.id && message.id < flush.startingId + flush.count) {
              this._attemptFlush(flush);
              break;
            }
          }
        }
        break;
      }
    }
  }

  private _attemptFlush(flush: SlingDisplayFlushMessage): void {
    const maxId = flush.startingId + flush.count;
    const messageParts = [];
    let displayType: SlingDisplayMessageType | undefined;
    for (let i = flush.startingId; i < maxId; ++i) {
      const displayMessage = this._displayBuffer.get(i);
      if (!displayMessage) {
        return;
      }
      if (displayType && displayType !== displayMessage.displayType) {
        // TODO handle this somehow
      } else if (!displayType) {
        displayType = displayMessage.displayType;
      }
      messageParts.push(displayMessage.value);
    }
    if (!displayType) {
      // TODO should not happen
      return;
    }

    this._queuedFlushes.delete(flush);
    for (let i = flush.startingId; i < maxId; ++i) {
      this._displayBuffer.delete(i);
    }
    const message = messageParts.map((x) => `${x}`).join('');
    this.emit('display', message, displayType);
  }
}
