# MMDX12 2.1.2 — Hotfix

> 🌐 **Language:** [한국어](#한국어) · [English](#english) · [日本語](#日本語) · [简体中文](#简体中文)

## 한국어
### 🐞 수정
- **재생 중 GPU 드라이버 오류로 앱이 종료되던 문제를 고쳤어요**: 움직이는 텍스처를 쓰는 MME 효과(예: 곡 전용 스테이지의 모니터 화면)와 셰이더 효과(보케 DoF 등)를 함께 켠 상태에서 창 크기를 바꾸거나 설정을 바꾼 뒤 씬을 다시 불러오면 "GPU 장치가 제거됨" 경고와 함께 종료될 수 있었어요. 프레임을 기록하는 도중 GPU 버퍼가 너무 일찍 해제되던 원인을 고쳤어요.

### 📦 안내
- 자동 업데이트로 받을 수 있어요(변경된 파일만 내려받아요).

## English
### 🐞 Fixes
- **Fixed the app closing with a GPU driver error during playback**: with an MME effect that uses an animated texture (e.g. the monitor screens of a song's own stage) and a shader effect (bokeh DoF and others) both on, resizing the window or reloading the scene after a settings change could stop the app with a "GPU device removed" warning. A GPU buffer was released too early while a frame was still being recorded; that is fixed.

### 📦 Notes
- Available through the automatic update (only the changed files are downloaded).

## 日本語
### 🐞 修正
- **再生中に GPU ドライバーのエラーでアプリが終了する問題を修正しました**: アニメーションテクスチャを使う MME エフェクト (曲専用ステージのモニター画面など) とシェーダーエフェクト (ボケ DoF など) を両方オンにした状態で、ウィンドウサイズを変えたり、設定を変えてシーンを再読み込みしたりすると、「GPU デバイスが削除されました」という警告とともに終了することがありました。フレームの記録中に GPU バッファーが早く解放されすぎていた原因を修正しました。

### 📦 案内
- 自動アップデートで入手できます (変更されたファイルのみダウンロードします)。

## 简体中文
### 🐞 修复
- **修复了播放时因 GPU 驱动错误导致应用退出的问题**: 同时开启使用动画纹理的 MME 效果 (例如歌曲专属舞台的显示屏画面) 和着色器效果 (散景景深等) 时,调整窗口大小或更改设置后重新加载场景,可能会弹出"GPU 设备已移除"警告并退出。原因是录制帧的过程中 GPU 缓冲区被过早释放,现已修复。

### 📦 说明
- 可通过自动更新获取 (只下载变更的文件)。
