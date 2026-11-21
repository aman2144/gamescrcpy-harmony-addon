import { UIAbility, Want } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { companion } from '../service/AudioCompanion';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want): void {
    const token = want.parameters?.['token'];
    if (typeof token === 'string') companion.configure(token, this.context);
  }
  onNewWant(want: Want): void {
    const token = want.parameters?.['token'];
    if (typeof token === 'string') companion.configure(token, this.context);
  }
  onWindowStageCreate(stage: window.WindowStage): void {
    stage.loadContent('pages/Index');
  }
  onDestroy(): void { companion.stop(); }
}
