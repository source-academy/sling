import { EventEmitter } from 'events';
import { MqttClient, connect as mqttConnect } from 'mqtt';
import { slingDeviceMessageTypes, deserialiseMqttMessage } from './slingProtocol';

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

export declare interface SlingClient {
  on(event: 'connect', listener: () => void): this;
  on(event: 'error', listener: (error: Error) => void): this;
}

export class SlingClient extends EventEmitter {
  options: SlingClientOptions;
  mqttClient?: MqttClient;

  constructor(options: SlingClientOptions) {
    super();
    this.options = options;
  }

  connect(): void {
    this.mqttClient = mqttConnect(this.options.websocketEndpoint, {
      clientId: this.options.clientId
    });
    this.mqttClient.on('connect', () => {
      this.handleConnect();
      this.emit('connect');
    });
    this.mqttClient.on('error', (error) => {
      this.emit('error', error);
    });
    this.mqttClient.on('message', (topic, payload) => {
      this.handleMessage(topic, payload);
    });
  }

  end(): void {
    if (!this.mqttClient) {
      return;
    }
    this.mqttClient.removeAllListeners();
    this.mqttClient.end();
  }

  private handleConnect(): void {
    this.mqttClient?.subscribe(
      slingDeviceMessageTypes.map((type) => `${this.options.deviceId}/${type}`),
      { qos: 1 }
    );
  }

  private handleMessage(topic: string, payload: Buffer): void {
    const message = deserialiseMqttMessage(topic, payload);
    if (!message) {
      return;
    }

    // TODO
  }
}
