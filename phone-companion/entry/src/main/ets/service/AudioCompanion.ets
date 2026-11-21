import { audio } from '@kit.AudioKit';
import { socket } from '@kit.NetworkKit';
import { util } from '@kit.ArkTS';
import { common, wantAgent } from '@kit.AbilityKit';
import { backgroundTaskManager } from '@kit.BackgroundTasksKit';

class AudioCompanion {
  private token: string = '';
  private capturer?: audio.AudioCapturer;
  private server?: socket.TCPSocketServer;
  private client?: socket.TCPSocketConnection;
  private active: boolean = false;
  private starting: boolean = false;
  private generation: number = 0;
  private context?: common.UIAbilityContext;
  private background: boolean = false;
  private busy: boolean = false;
  private queuedBytes: number = 0;
  private pending: ArrayBuffer[] = [];
  private message: string = '请从电脑启动鸿蒙实验版';
  onStatus?: (text: string) => void;

  configure(token: string, context: common.UIAbilityContext): void {
    if (!/^[a-f0-9]{64}$/.test(token)) return;
    if (token !== this.token && this.token.length > 0) this.stop();
    this.token = token;
    this.context = context;
    if (!this.active) this.status('已连接电脑，可开启声音传输');
  }
  status(text: string): void {
    this.message = text;
    this.onStatus?.(text);
  }
  getStatus(): string { return this.message; }

  async testTone(): Promise<void> {
    const renderer = await audio.createAudioRenderer({
      streamInfo: { samplingRate: audio.AudioSamplingRate.SAMPLE_RATE_48000,
        channels: audio.AudioChannel.CHANNEL_2, sampleFormat: audio.AudioSampleFormat.SAMPLE_FORMAT_S16LE,
        encodingType: audio.AudioEncodingType.ENCODING_TYPE_RAW },
      rendererInfo: { usage: audio.StreamUsage.STREAM_USAGE_MUSIC, rendererFlags: 0 }
    });
    try {
      await renderer.setVolume(0.15);
      await renderer.start();
      const samples = new Int16Array(48000 * 2);
      for (let i = 0; i < 48000; ++i) {
        const envelope = Math.min(1, i / 480, (48000 - i) / 480);
        const value = Math.round(12000 * envelope * Math.sin(2 * Math.PI * 440 * i / 48000));
        samples[i * 2] = value; samples[i * 2 + 1] = value;
      }
      let offset: number = 0;
      while (offset < samples.buffer.byteLength) {
        const written = await renderer.write(samples.buffer.slice(offset));
        if (written <= 0) throw new Error('Test tone renderer stopped');
        offset += written;
      }
      await renderer.drain();
    } finally { await renderer.release(); }
  }

  async start(): Promise<void> {
    if (this.active || this.capturer || this.starting) return;
    if (this.token.length !== 64) { this.status('请先从电脑启动鸿蒙实验版'); return; }
    const generation = ++this.generation;
    this.starting = true;
    try {
      this.status('等待系统授权');
      this.capturer = await audio.createAudioCapturer({
        streamInfo: { samplingRate: audio.AudioSamplingRate.SAMPLE_RATE_48000,
          channels: audio.AudioChannel.CHANNEL_2, sampleFormat: audio.AudioSampleFormat.SAMPLE_FORMAT_S16LE,
          encodingType: audio.AudioEncodingType.ENCODING_TYPE_RAW },
        capturerInfo: { source: audio.SourceType.SOURCE_TYPE_PLAYBACK_CAPTURE, capturerFlags: 0 },
        playbackCaptureMode: audio.AudioPlaybackCaptureMode.MODE_DEFAULT
      });
      if (generation !== this.generation) { await this.capturer.release(); this.capturer = undefined; return; }
      this.capturer.on('readData', (data: ArrayBuffer) => {
        if (generation === this.generation) this.enqueue(data);
      });
      this.capturer.on('stateChange', (state: audio.AudioState) => {
        if (this.active && state !== audio.AudioState.STATE_RUNNING) this.stop();
      });
      this.capturer.requestPlaybackCaptureStart((state: audio.PlaybackCaptureStartState) => {
        if (generation !== this.generation) return;
        if (state === audio.PlaybackCaptureStartState.STATE_SUCCESS) {
          this.active = true;
          this.startSession(generation).catch(() => { this.stop(); this.status('声音连接失败，请重新开启'); });
        } else {
          this.stop();
          this.status(state === audio.PlaybackCaptureStartState.STATE_NOT_AUTHORIZED ?
            '未获得系统录音授权' : '系统未能开启声音采集');
        }
      });
    } catch (error) { await this.stop(); this.status('声音采集失败：' + JSON.stringify(error)); }
    finally { this.starting = false; }
  }

  private async startSession(generation: number): Promise<void> {
    if (!this.context) throw new Error('Ability context is unavailable');
    const agent = await wantAgent.getWantAgent({ wants: [{ bundleName: 'org.example.gamescrcpy.audio',
      abilityName: 'EntryAbility' }], operationType: wantAgent.OperationType.START_ABILITY,
      requestCode: 0, wantAgentFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG] });
    if (generation !== this.generation) return;
    await backgroundTaskManager.startBackgroundRunning(this.context,
      backgroundTaskManager.BackgroundMode.AUDIO_RECORDING, agent);
    this.background = true;
    if (generation !== this.generation) { await this.stop(); return; }
    await this.listen();
  }

  private async listen(): Promise<void> {
    this.server = socket.constructTCPSocketServerInstance();
    this.server.on('connect', (connection: socket.TCPSocketConnection) => {
      if (this.client) { connection.close(); return; }
      let greeting: string = '';
      let authenticated: boolean = false;
      const timer = setTimeout(() => { if (!authenticated) connection.close(); }, 5000);
      connection.on('message', (event: socket.SocketMessageInfo) => {
        if (authenticated) return;
        if (greeting.length + event.message.byteLength > 128) { connection.close(); return; }
        greeting += new util.TextDecoder('utf-8').decodeToString(new Uint8Array(event.message));
        if (!greeting.includes('\n')) return;
        if (greeting !== 'GSA1 ' + this.token + '\n') { connection.close(); return; }
        authenticated = true;
        clearTimeout(timer);
        if (this.client) { connection.close(); return; }
        connection.send({ data: 'GSA1 48000 2 S16LE\n' }).then(() => {
          if (!this.active || this.client) { connection.close(); return; }
          this.client = connection;
          this.status('正在向电脑传输声音');
        }).catch(() => { connection.close(); });
      });
      connection.on('close', () => {
        clearTimeout(timer);
        if (this.client === connection) {
          this.client = undefined; this.pending = []; this.queuedBytes = 0;
          this.status('声音采集中，等待电脑连接');
        }
      });
      connection.on('error', () => { connection.close(); });
    });
    await this.server.listen({ address: '127.0.0.1', port: 52782, family: 1 });
    this.status('声音采集中，等待电脑连接');
  }

  private enqueue(data: ArrayBuffer): void {
    if (!this.active || !this.client || data.byteLength === 0) return;
    if (data.byteLength % 4 !== 0 || data.byteLength > 192000) { this.stop(); return; }
    // Bound playback latency to 250 ms; stop instead of accumulating stale sound.
    if (this.queuedBytes + data.byteLength > 48000) {
      this.client.close(); this.status('声音连接过慢，正在等待重连'); return;
    }
    const packet = new ArrayBuffer(data.byteLength + 4);
    new DataView(packet).setUint32(0, data.byteLength, false);
    new Uint8Array(packet, 4).set(new Uint8Array(data));
    this.pending.push(packet); this.queuedBytes += data.byteLength;
    this.flush();
  }

  private async flush(): Promise<void> {
    if (this.busy) return;
    this.busy = true;
    const connection = this.client;
    try {
      while (connection && this.client === connection && this.pending.length > 0) {
        const packet = this.pending.shift();
        if (packet) {
          this.queuedBytes -= packet.byteLength - 4;
          await connection.send({ data: packet });
        }
      }
    } catch (_) { connection?.close(); }
    finally { this.busy = false; }
  }

  async stop(): Promise<void> {
    ++this.generation;
    this.active = false;
    const capture = this.capturer; this.capturer = undefined;
    this.client?.close(); this.client = undefined;
    this.server?.close(); this.server = undefined;
    this.pending = []; this.queuedBytes = 0;
    if (this.background && this.context) {
      this.background = false;
      try { await backgroundTaskManager.stopBackgroundRunning(this.context); } catch (_) { }
    }
    if (capture) {
      try { await capture.release(); } catch (_) { }
    }
    this.status('声音传输已关闭');
  }
}
export const companion = new AudioCompanion();
