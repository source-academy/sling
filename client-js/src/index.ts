import { EventEmitter } from 'events';
import { MqttClient, connect as mqttConnect } from 'mqtt';

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
      this.emit('connect');
    });
  }
}
