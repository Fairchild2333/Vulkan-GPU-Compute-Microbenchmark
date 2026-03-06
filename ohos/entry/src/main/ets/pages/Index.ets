import { hilog } from '@kit.PerformanceAnalysisKit';

const TAG = 'GpuBench';

@Entry
@Component
struct Index {
  private xComponentController: XComponentController = new XComponentController();

  build() {
    Stack() {
      XComponent({
        id: 'vulkan_surface',
        type: XComponentType.SURFACE,
        libraryname: 'gpubench',
        controller: this.xComponentController
      })
        .onLoad(() => {
          hilog.info(0x0000, TAG, 'XComponent onLoad — native surface created');
        })
        .onDestroy(() => {
          hilog.info(0x0000, TAG, 'XComponent onDestroy — native surface destroyed');
        })
        .width('100%')
        .height('100%')

      Text('GPU Compute Benchmark — Vulkan')
        .fontSize(16)
        .fontColor('#88FFFFFF')
        .position({ x: 16, y: 16 })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#0A1424')
  }
}
