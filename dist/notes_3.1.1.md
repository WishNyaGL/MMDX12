# MMDX12 3.1.1 — Inverse-Square Light Fix

> 🌐 **Language:** [한국어](#한국어) · [English](#english) · [日本語](#日本語) · [简体中文](#简体中文)

## 한국어
### 🐞 수정 및 개선
- **감쇠를 '역제곱'으로 설정한 조명이 거의 보이지 않던 문제를 고쳤어요**: 역제곱 감쇠가 기준 거리 없이 계산돼서, 보통 강도(1~2)의 조명이 몇 미터만 떨어져도 수백 분의 일 밝기로 어두워졌어요. 이제 강도는 모든 감쇠 방식에서 2 m(25) 거리의 밝기를 뜻해서, 감쇠 방식을 바꿔도 같은 강도 값이 비슷한 밝기로 보여요. 광원 바로 옆의 밝기는 최대 4배로 제한돼요. 래스터·레이 트레이싱·패스 트레이싱·GI 렌더와 안개 빛줄기에 모두 적용돼요.
- **역제곱 조명을 쓰는 스테이지가 원래 의도대로 밝아졌어요**: '미러 안무 연습실'의 벽면·로고·플로어 스팟과 '컬러 코브 스튜디오'의 호리존트 색 조명이 이제 제대로 비쳐요.

### 📦 안내
- 자동 업데이트로 받을 수 있어요(변경된 파일만 내려받아요).
- '미러 안무 연습실' 스테이지 1.0.1(바닥에 검은 얼룩이 보이던 문제 수정)은 갤러리에서 업데이트할 수 있어요.

## English
### 🐞 Fixes
- **Lights set to the inverse-square falloff were almost invisible**: the inverse-square curve had no reference distance, so a light of an ordinary intensity (1-2) dropped to a few hundredths of its brightness a few metres away. Intensity now means the brightness at 2 m (25 units) for every falloff curve, so switching the curve keeps the same intensity looking about as bright. The brightness right next to the source is capped at 4x. Applies to raster, ray tracing, path tracing, GI renders and haze beams.
- **Stages using inverse-square lights are lit as designed**: the wall, logo and floor spots of "Mirror Practice Room" and the cyc colour lights of "Color Cove Studio" now shine properly.

### 📦 Notes
- Available through the automatic updater (only changed files are downloaded).
- The "Mirror Practice Room" stage 1.0.1 (fixes black blotches on the floor) is available as an update in the gallery.

## 日本語
### 🐞 修正
- **減衰を「逆二乗」にしたライトがほとんど見えない問題を修正しました**: 逆二乗の減衰が基準距離なしで計算されていたため、通常の強度（1〜2）のライトが数メートル離れるだけで数百分の一の明るさになっていました。強度はすべての減衰方式で 2 m（25）の距離での明るさを表すようになり、減衰方式を切り替えても同じ強度でほぼ同じ明るさになります。光源のすぐそばの明るさは最大 4 倍に制限されます。ラスター・レイトレーシング・パストレーシング・GI レンダーとフォグの光の筋すべてに適用されます。
- **逆二乗ライトを使うステージが本来の明るさになりました**: 「ミラーダンス練習室」の壁面・ロゴ・フロアスポットと「カラーコーブ・スタジオ」のホリゾント色ライトが正しく照らすようになりました。

### 📦 お知らせ
- 自動アップデートで入手できます（変更されたファイルのみダウンロード）。
- 「ミラーダンス練習室」ステージ 1.0.1（床に黒いしみが出る問題の修正）はギャラリーから更新できます。

## 简体中文
### 🐞 修复
- **修复了衰减设为"平方反比"的灯光几乎看不见的问题**：平方反比衰减没有参考距离，普通强度（1~2）的灯光离开几米就只剩几百分之一的亮度。现在强度在所有衰减方式下都表示 2 m（25）距离处的亮度，切换衰减方式后同样的强度值看起来亮度相近。光源旁边的亮度最多限制为 4 倍。适用于光栅、光线追踪、路径追踪、GI 渲染和雾中光束。
- **使用平方反比灯光的舞台恢复了设计中的亮度**："镜面舞蹈练习室"的墙面、标志和地面射灯，以及"色彩弧幕摄影棚"的天幕彩色灯光现在都能正常照亮。

### 📦 说明
- 可通过自动更新获取（只下载更改过的文件）。
- "镜面舞蹈练习室"舞台 1.0.1（修复地面出现黑色斑块的问题）可在画廊中更新。
