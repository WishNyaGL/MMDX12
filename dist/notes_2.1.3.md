<img src="https://raw.githubusercontent.com/digital8150/MMDX12/master/docs/media/physics-2.1.3.jpg" alt="MMDX12 2.1.3 - Physics mode-2 chain fix (ZZZ / Star Rail)" width="100%">

# MMDX12 2.1.3 — Bug fixes & Improvements

> 🌐 **Language:** [한국어](#한국어) · [English](#english) · [日本語](#日本語) · [简体中文](#简体中文)

## 한국어
### 🐞 수정 및 개선
- **모드 2 물리 본(옷자락·소매·머리카락 체인)이 굳어있던 문제를 고쳤어요**: 젠레스 존 제로(ZZZ)나 붕괴: 스타레일 등 최신 게임 모델의 긴 코트, 소매, 머리카락처럼 '물리+위치 맞춤'(모드 2) 바디가 체인으로 연결된 모델에서, 이전에는 월드 위치를 애니메이션 절대 좌표로 덮어써 회전만 하고 위치가 따라가지 않아 옷자락이 굳어 보였어요. 이제 부모 본의 물리 계산 결과를 바탕으로 계층 위치를 올바르게 계산하여 옷과 머리카락이 자연스럽게 흩날려요(이슈 #4).
- **HDD에서 라이브러리 스캔이 빨라졌어요**: 파일 정보를 읽을 때 불필요한 시스템 호출을 줄이고, 하드 디스크(HDD) 환경에서는 파일들을 순차적으로 미리 읽어오는(SequentialPrefetch) 기능을 적용해 라이브러리 스캔 시 헤드 탐색 낭비를 줄였어요.
- **영상 렌더링 구간 지정 (HH:MM:SS)**: 고화질 영상 렌더링 창에서 곡 전체뿐 아니라 원하는 시작/종료 시점을 타임코드(예: 0:12 ~ 1:05)로 지정해 필요한 부분만 렌더링할 수 있어요.

### 📦 안내
- 자동 업데이트로 받을 수 있어요(변경된 파일만 내려받아요).

## English
### 🐞 Fixes & Improvements
- **Fixed stiff clothing, sleeve, and hair chains with Mode 2 physics bones**: in models with chains of 'physics + align position' (mode 2) bodies—such as coats, sleeves, and hair on Zenless Zone Zero (ZZZ) and Honkai: Star Rail models—bones previously retained their pre-physics animated world translations while rotating, causing stiff or detached motion. World positions are now properly reconstructed through the bone hierarchy from the parent's post-physics transform, allowing clothes and hair to flow naturally (resolves issue #4).
- **Faster library scanning on HDDs**: redundant per-file stat system calls during scanning have been eliminated, and on storage drives with seek penalties (HDDs), files are pre-read sequentially along path order to prevent disk thrashing.
- **Video render custom time range (HH:MM:SS)**: the high-quality video dialog now allows specifying custom start and end timecodes (e.g. 0:12 ~ 1:05) in addition to rendering the entire song.

### 📦 Notes
- Available through the automatic update (only the changed files are downloaded).

## 日本語
### 🐞 修正および改善
- **モード2物理ボーン (裾・袖・髪の毛チェーン) が固まる問題を修正**: ゼンレスゾーンゼロ (ZZZ) や崩壊: スターレイルなどのモデルに見られる長いコート、袖、髪のように「物理+位置合わせ」(モード2) が連続する構造で、アニメーションの絶対位置で上書きされていたため位置が追従せず固まって見えていた問題を修正しました。親ボーンの物理結果から階層位置を再構築することで、服や髪が自然になびくようになりました (Issue #4)。
- **HDD でのライブラリスキャン高速化**: ディレクトリ走査時の不要な stat 呼び出しを削減し、HDD などのシーク遅延がある環境ではパス順に先行読み込み (SequentialPrefetch) を行うことでスキャン速度を大幅に改善しました。
- **動画レンダリングの区間指定 (HH:MM:SS)**: 高画質動画レンダリングダイアログで、曲全体だけでなく指定したタイムコード (例: 0:12 ～ 1:05) の区間のみを書き出せるようになりました。

### 📦 案内
- 自動アップデートで入手できます (変更されたファイルのみダウンロードします)。

## 简体中文
### 🐞 修复与改进
- **修复了模式2物理骨骼 (下摆、衣袖、发丝链) 僵硬的问题**: 在绝区零 (ZZZ) 或崩坏: 星穹铁道等模型中，带有连续“物理+对齐位置”(模式2) 刚体的长风衣、衣袖和头发链，此前因直接套用动画绝对世界坐标导致只转动而不随位置移动、显得僵硬。现已通过父级物理结果重新构建骨骼层级位移，使衣物和发丝能够自然飘动 (解决 Issue #4)。
- **机械硬盘 (HDD) 资源库扫描提速**: 减少了扫描时的多余文件状态系统调用，并在存在寻道延迟的机械硬盘上按路径顺序进行预读取 (SequentialPrefetch)，显著减少了磁盘寻道开销。
- **视频渲染区间指定 (HH:MM:SS)**: 高画质视频渲染窗口现在支持通过时间码 (如 0:12 ~ 1:05) 指定起始与结束时间，仅渲染指定片段。

### 📦 说明
- 可通过自动更新获取 (只下载变更的文件)。
