import { companion } from '../service/AudioCompanion';

@Entry
@Component
struct Index {
  @State status: string = companion.getStatus();
  @State currentTouches: number = 0;
  @State maxTouches: number = 0;
  private contacts: Set<number> = new Set<number>();
  aboutToAppear(): void { companion.onStatus = (text: string) => { this.status = text; }; }
  aboutToDisappear(): void { companion.onStatus = undefined; }
  build() {
    Column({ space: 24 }) {
      Text('GameScrcpy 声音助手').fontSize(26).fontWeight(FontWeight.Bold)
      Text('将手机播放的声音通过 USB 传到电脑').fontSize(18)
      Text(this.status).fontSize(17).fontColor('#345A80')
      Button('开启声音传输').onClick(() => { companion.start(); })
      Button('关闭声音传输').onClick(() => { companion.stop(); })
      Button('播放测试音').onClick(() => { companion.testTone().catch(() => { this.status = '测试音播放失败'; }); })
      Column({ space: 12 }) {
        Text('触控测试区').fontSize(18)
        Text('当前 ' + this.currentTouches + ' 指，最多 ' + this.maxTouches + ' 指').fontSize(17)
      }.width('100%').height(160).justifyContent(FlexAlign.Center).backgroundColor('#EAF2FF')
      .onTouch((event: TouchEvent) => {
        for (const point of event.changedTouches) {
          if (event.type === TouchType.Down || event.type === TouchType.Move) this.contacts.add(point.id);
          else this.contacts.delete(point.id);
        }
        if (event.type === TouchType.Cancel) this.contacts.clear();
        this.currentTouches = this.contacts.size;
        this.maxTouches = Math.max(this.maxTouches, this.currentTouches);
      })
      Text('开启时请允许系统弹出的声音采集请求。传输只面向当前连接的电脑。').fontSize(15).fontColor('#666666')
    }.padding(28).justifyContent(FlexAlign.Center).width('100%').height('100%')
  }
}
