# MMDX12 2.1.1 — Bug fix

> 🌐 **Language:** [한국어](#한국어) · [English](#english) · [日本語](#日本語) · [简体中文](#简体中文)

## 한국어
### 🐞 수정
- **GPU 드라이버 오류로 앱이 종료되던 문제를 고쳤어요**: 움직이는 텍스처(ANIMATEDTEXTURE)를 쓰는 MME 효과(예: 곡 전용 스테이지의 모니터 화면)가 있으면 재생을 시작하자마자 "GPU 장치가 제거됨" 경고와 함께 종료될 수 있었어요. 텍스처 업로드 방식을 고쳐 이제 정상적으로 재생돼요.
- **여러 명이 나오는 유닛 곡이 더 부드러워졌어요**: 출연자들의 포즈를 병렬로 계산하고, 프레임이 느려질 때 물리 연산이 계속 밀리며 끊기던 현상을 막았어요(오프라인 렌더는 이전과 같은 정밀도).
- 거리 값이 0 근처를 오가는 카메라 모션에서 화면이 순간적으로 뒤집히던 문제를 고쳤어요.
- 배경·오버레이 영상이 가끔 프레임을 건너뛰던 문제를 고쳤어요.

### 📦 안내
- 자동 업데이트로 받을 수 있어요(변경된 파일만 약 4 MB 내려받아요).

## English
### 🐞 Fixes
- **Fixed the app closing with a GPU driver error**: an MME effect with an animated texture (ANIMATEDTEXTURE, e.g. the monitor screens of a song's own stage) could stop playback right at the start with a "GPU device removed" warning. The texture upload is fixed and these scenes play normally.
- **Smoother unit songs with many performers**: performers are posed in parallel, and a slow frame no longer makes physics fall further and further behind (offline renders keep the full precision).
- Fixed a camera motion briefly flipping the view when its distance hovers around 0.
- Fixed background / overlay videos occasionally skipping frames.

### 📦 Notes
- Available through the automatic update (only the changed files, about 4 MB).

## 日本語
### 🐞 修正
- **GPU ドライバーのエラーでアプリが終了する問題を修正**: アニメーションテクスチャ (ANIMATEDTEXTURE) を使う MME エフェクト (曲専用ステージのモニター画面など) があると、再生直後に「GPU デバイスが削除されました」という警告とともに終了することがありました。テクスチャのアップロードを修正し、正常に再生できるようになりました。
- **大人数のユニット曲がより滑らかに**: 出演者のポーズを並列で計算し、フレームが遅れたときに物理演算がどんどん遅れていく現象を防ぎました (オフラインレンダーの精度は従来どおり)。
- 距離が 0 付近を行き来するカメラモーションで、画面が一瞬反転する問題を修正しました。
- 背景・オーバーレイ映像がときどきフレームを飛ばす問題を修正しました。

### 📦 案内
- 自動アップデートで入手できます (変更されたファイルのみ、約 4 MB)。

## 简体中文
### 🐞 修复
- **修复了因 GPU 驱动错误导致应用退出的问题**: 使用动画纹理 (ANIMATEDTEXTURE) 的 MME 效果 (例如歌曲专属舞台的显示屏画面) 可能在开始播放时立即弹出"GPU 设备已移除"警告并退出。现已修复纹理上传,这些场景可以正常播放。
- **多人组合歌曲更流畅**: 表演者的姿势改为并行计算,并防止帧变慢时物理运算越积越多 (离线渲染保持原有精度)。
- 修复了距离在 0 附近来回变化的镜头动作会让画面瞬间翻转的问题。
- 修复了背景 / 叠加视频偶尔跳帧的问题。

### 📦 说明
- 可通过自动更新获取 (只下载变更的文件,约 4 MB)。
