# MMDX12 3.1.0 — Studio Workflow Update

> 🌐 **Language:** [한국어](#한국어) · [English](#english) · [日本語](#日本語) · [简体中文](#简体中文)

## 한국어
### ✨ 새 기능
- **타임라인에 음원 행이 생겼어요**: 음원이 눈금자 아래의 고정된 행에 클립과 파형으로 표시돼요. 클립을 드래그하면 오프셋이 바뀌고(재생 헤드와 0프레임에 붙고, Shift를 누르면 자유롭게 움직여요), 실행 취소도 돼요. 행을 우클릭하면 오프셋 입력, 재생 헤드에서 시작, 오프셋 초기화, 음원 바꾸기·제거 메뉴가 열려요.
- **프로그램 설정 창이 생겼어요**: 카메라 회전·이동·줌·화각 감도와 축 반전, Ctrl 정밀 조작 배율, 본 도구 단축키(W / E / K / C / I, Space) 변경을 한 곳에서 할 수 있어요.
- **본·모프의 영어 이름을 표시할 수 있어요**: PMX에 영어 이름이 있으면 포즈 탭, 타임라인, 아웃라이너에 영어 이름으로 보여요. VMD·VPD 파일에는 원래 이름이 그대로 저장돼요.
- **스튜디오 본 편집 도구가 좋아졌어요**: 뷰포트에서 본을 축별로 이동·회전하는 패널, 겹친 관절을 다시 클릭하면 차례로 선택, 클릭한 본으로 카메라 중심 이동, 본 오버레이 크기 설정, Shift 다중 선택, Ctrl+A 전체 선택을 쓸 수 있어요.
- **스튜디오에 바람과 물리 옵션이 생겼어요**: 바람 방향·세기·돌풍·무작위성을 조절해 머리카락과 치마를 흔들 수 있고, 멈춰 있을 때도 물리 계산하기, 물리를 꺼도 물리 본 표시하기를 켤 수 있어요.
- **라이브러리를 목록으로 볼 수 있어요**: 격자 / 목록 보기 전환, 열 수 조절, 숨긴 항목 목록에서 하나씩 되돌리기를 지원해요.

### 🐞 수정 및 개선
- **실시간 렌더러의 영상 렌더 남은 시간이 정확해졌어요**: 래스터·레이 트레이싱·패스 트레이싱으로 영상을 렌더할 때 남은 시간과 렌더 창의 예상 시간이 실제 속도에 맞게 계산돼요.
- **파형을 백그라운드에서 분석해요**: 긴 음원을 넣어도 스튜디오가 멈추지 않아요.

### 🙏 기여
- 설정 창, 영어 이름, 파형, 라이브러리 목록 보기, 본 도구, 바람 물리는 WishNyaGL 님의 기여(이슈 #5)예요. 감사합니다!

### 📦 안내
- 자동 업데이트로 받을 수 있어요(변경된 파일만 내려받아요).

## English
### ✨ What's New
- **The timeline has an audio lane**: the audio shows as a clip with its waveform in a fixed lane under the ruler. Drag the clip to change the offset (it snaps to the playhead and frame 0; hold Shift to move freely), with undo. Right-click the lane for the offset field, start at the playhead, reset the offset, and replace / remove the audio.
- **A program settings window**: camera rotate / pan / zoom / FOV sensitivity and axis inversion, the Ctrl precision factor, and rebindable bone tool keys (W / E / K / C / I, Space) in one place.
- **English bone and morph names**: when a PMX has English names, the pose tab, the timeline and the outliner can show them. VMD and VPD files still store the original names.
- **Better studio bone tools**: a per-axis move / rotate panel in the viewport, repeated clicks cycle through overlapping joints, center the camera on the clicked bone, a bone overlay size setting, Shift multi-select and Ctrl+A select all.
- **Wind and physics options in the studio**: wind direction, strength, gusts and randomness move hair and skirts; physics can keep simulating while paused, and physics bones can stay visible with physics off.
- **A list view for the library**: grid / list switch, column count, and a hidden items list where each item can be restored.

### 🐞 Fixes
- **Video render time estimates are accurate for the real-time renderers**: rendering a video with raster, ray tracing or path tracing now estimates the remaining time and the render dialog's estimate from the real frame rate.
- **The waveform is analyzed in the background**: long audio files no longer freeze the studio.

### 🙏 Thanks
- The settings window, English names, waveform, library list view, bone tools and wind physics were contributed by WishNyaGL (issue #5). Thank you!

### 📦 Notes
- Available through the automatic updater (only changed files are downloaded).

## 日本語
### ✨ 新機能
- **タイムラインに音源トラックが追加されました**: 音源がルーラー下の固定トラックにクリップと波形で表示されます。クリップをドラッグするとオフセットが変わり（再生ヘッドと0フレームにスナップ、Shiftで自由移動）、元に戻すこともできます。トラックを右クリックすると、オフセット入力、再生ヘッドから開始、オフセットのリセット、音源の差し替え・削除メニューが開きます。
- **プログラム設定ウィンドウが追加されました**: カメラの回転・移動・ズーム・画角の感度と軸反転、Ctrl精密操作の倍率、ボーンツールのショートカット（W / E / K / C / I、Space）の変更を一か所で行えます。
- **ボーン・モーフの英語名を表示できます**: PMXに英語名がある場合、ポーズタブ、タイムライン、アウトライナーに英語名で表示されます。VMD・VPDファイルには元の名前のまま保存されます。
- **スタジオのボーン編集ツールが改善されました**: ビューポートでボーンを軸ごとに移動・回転するパネル、重なった関節を再クリックで順に選択、クリックしたボーンへのカメラ中心移動、ボーンオーバーレイのサイズ設定、Shiftでの複数選択、Ctrl+Aでの全選択が使えます。
- **スタジオに風と物理のオプションが追加されました**: 風の方向・強さ・突風・ランダム性を調整して髪やスカートを揺らせます。停止中も物理演算を続ける設定と、物理をオフにしても物理ボーンを表示する設定もあります。
- **ライブラリをリストで表示できます**: グリッド / リスト表示の切り替え、列数の調整、非表示項目リストからの個別の復元に対応しました。

### 🐞 修正
- **リアルタイムレンダラーの動画レンダリング残り時間が正確になりました**: ラスター・レイトレーシング・パストレーシングで動画をレンダリングするとき、残り時間とレンダーダイアログの予想時間が実際の速度に合わせて計算されます。
- **波形をバックグラウンドで解析します**: 長い音源を読み込んでもスタジオが止まりません。

### 🙏 謝辞
- 設定ウィンドウ、英語名、波形、ライブラリのリスト表示、ボーンツール、風の物理は WishNyaGL さんの貢献（Issue #5）です。ありがとうございます！

### 📦 お知らせ
- 自動アップデートで入手できます（変更されたファイルのみダウンロード）。

## 简体中文
### ✨ 新功能
- **时间轴新增音频轨**：音频以片段和波形显示在标尺下方的固定轨道中。拖动片段即可修改偏移（吸附到播放头和第0帧，按住 Shift 可自由移动），并支持撤销。右键单击轨道可打开偏移输入、从播放头开始、重置偏移、替换 / 移除音频菜单。
- **新增程序设置窗口**：可在一处调整相机旋转 / 平移 / 缩放 / 视角灵敏度和轴反转、Ctrl 精细操作倍率，以及骨骼工具快捷键（W / E / K / C / I、Space）。
- **可显示骨骼和变形的英文名称**：PMX 带有英文名称时，姿势标签、时间轴和大纲中可显示英文名称。VMD / VPD 文件仍保存原始名称。
- **工作室骨骼编辑工具改进**：视口中可按轴移动 / 旋转骨骼的面板、重复点击依次选择重叠的关节、相机中心移动到所点击的骨骼、骨骼覆盖层大小设置、Shift 多选和 Ctrl+A 全选。
- **工作室新增风和物理选项**：调整风向、强度、阵风和随机性来吹动头发和裙子；还可以在暂停时继续物理计算，以及在关闭物理时仍显示物理骨骼。
- **资源库支持列表视图**：网格 / 列表视图切换、列数调整，以及可逐项恢复的隐藏项目列表。

### 🐞 修复
- **实时渲染器的视频渲染剩余时间更准确了**：使用光栅、光线追踪或路径追踪渲染视频时，剩余时间和渲染对话框的预计时间会按实际速度计算。
- **波形在后台分析**：载入较长的音频时工作室不再卡住。

### 🙏 致谢
- 设置窗口、英文名称、波形、资源库列表视图、骨骼工具和风物理由 WishNyaGL 贡献（Issue #5）。谢谢！

### 📦 说明
- 可通过自动更新获取（只下载更改过的文件）。
