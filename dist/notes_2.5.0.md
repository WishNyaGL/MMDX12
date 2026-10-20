# MMDX12 2.5.0 — Stage Shadows & World Effects Everywhere

> 🌐 **Language:** [한국어](#한국어) · [English](#english) · [日本語](#日本語) · [简体中文](#简体中文)

## 한국어
### ✨ 새 기능
- **눈 이펙트가 맵 전체에 내리고 실제 무대 표면 위에 쌓여요**: 카메라가 보는 곳 어디든 눈이 내리고, 단차나 높은 보도도 따라 쌓여요. 벽과 허공에는 쌓이지 않고, 발자국은 캐릭터 주변에 남아요.
- **월드 이펙트 팩이 카메라·캐릭터·실제 지면 기준으로 동작할 수 있어요**: 팩 제작자용 기능 `world.camera` / `world.anchor` / `world.ground` / `world.overlay`가 추가됐어요(문서: 월드 이펙트).
- **갤러리에 비·땅안개·반딧불 월드 이펙트가 추가됐어요**: 비는 무대 원래 바닥을 젖게 하고 평평한 곳에 웅덩이를 만들어요.
- **MCP로 월드 이펙트와 플레이 카메라를 조정할 수 있어요**: `set_render_settings`의 `world_stack`, 새 도구 `set_camera`.

### 🐞 수정 및 개선
- **무대에 캐릭터 그림자가 생겨요**: 그림자 설정이 비어 있는 무대(게임에서 추출한 무대 등)도 이제 그림자를 받아요. 제작자가 설정한 무대는 그 설정을 따라요.
- **스포트라이트 그림자가 또렷해졌어요**: 부드러운 그림자가 지나치게 퍼져 사라지던 문제를 고쳤어요(래스터·RT·PT·오프라인 GI 공통).
- **월드 이펙트가 무대 조명(스포트·포인트)을 받아요**: 쌓인 눈과 젖은 바닥이 조명과 그 그림자를 받아요.
- **곡 전용 스테이지가 있는 곡에서도 스튜디오 바닥을 고를 수 있어요**: "스튜디오 (스테이지 없음)"을 고르면 저장돼요.

### 📦 안내
- 자동 업데이트로 받을 수 있어요(변경된 파일만 내려받아요).

## English
### ✨ New
- **Snow falls over the whole map and settles on the real stage surfaces**: it falls wherever the camera looks and follows steps and raised walkways; no snow on walls or in mid-air; footprints stay around the characters.
- **World effect packs can work around the camera, the performers and the real ground**: new pack capabilities `world.camera` / `world.anchor` / `world.ground` / `world.overlay` (docs: World Effects).
- **Rain, Ground Mist and Fireflies world effects are in the gallery**: rain wets the stage's own floor and leaves puddles on flat ground.
- **MCP can set the world effects and the play camera**: `world_stack` in `set_render_settings` and the new `set_camera` tool.

### 🐞 Fixes
- **Stages now show the characters' shadows**: stages with empty shadow settings (e.g. game rips) receive shadows; stages whose authors set them keep their settings.
- **Spotlight shadows are crisp again**: soft shadows no longer blur away (raster, RT, PT and offline GI).
- **World effects receive the stage lights (spots / points)**: snow cover and wet floors take the lights and their shadows.
- **The studio floor can be chosen for songs with their own stage**: pick "Studio (no stage)" and it is saved.

### 📦 Notes
- Available through the automatic updater (only changed files are downloaded).

## 日本語
### ✨ 新機能
- **雪がマップ全体に降り、実際のステージの表面に積もります**: カメラが映すどこにでも降り、段差や高い歩道にも沿って積もります。壁や空中には積もらず、足跡はキャラクターの周りに残ります。
- **ワールドエフェクトパックがカメラ・キャラクター・実際の地面を基準に動作できます**: パック制作者向け機能 `world.camera` / `world.anchor` / `world.ground` / `world.overlay` を追加しました（ドキュメント: ワールドエフェクト）。
- **ギャラリーに雨・地霧・ホタルのワールドエフェクトを追加しました**: 雨はステージ本来の床を濡らし、平らな場所に水たまりを作ります。
- **MCP でワールドエフェクトとプレイカメラを設定できます**: `set_render_settings` の `world_stack` と新しいツール `set_camera`。

### 🐞 修正
- **ステージにキャラクターの影が落ちるようになりました**: 影の設定が空のステージ（ゲームからの抽出など）も影を受けます。制作者が設定したステージはその設定に従います。
- **スポットライトの影がくっきりしました**: ソフトシャドウが広がりすぎて消える問題を修正しました（ラスター・RT・PT・オフライン GI 共通）。
- **ワールドエフェクトがステージの照明（スポット・ポイント）を受けます**: 積雪や濡れた床が照明とその影を受けます。
- **専用ステージのある曲でもスタジオの床を選べます**: 「スタジオ（ステージなし）」を選ぶと保存されます。

### 📦 お知らせ
- 自動アップデートで入手できます（変更されたファイルのみダウンロード）。

## 简体中文
### ✨ 新功能
- **雪会落满整张地图，并积在真实的舞台表面上**：镜头所及之处都会下雪，并沿台阶和高处的人行道积雪；墙面和半空中不会积雪，脚印留在角色周围。
- **世界特效包可以基于镜头、角色和真实地面工作**：新增面向包作者的功能 `world.camera` / `world.anchor` / `world.ground` / `world.overlay`（文档：世界特效）。
- **图库新增雨、地雾和萤火虫世界特效**：雨会打湿舞台本来的地面，并在平坦处形成水洼。
- **可以通过 MCP 设置世界特效和播放镜头**：`set_render_settings` 的 `world_stack` 以及新工具 `set_camera`。

### 🐞 修复
- **舞台上会出现角色的影子**：阴影设置为空的舞台（如从游戏中提取的舞台）现在也会接收阴影；作者已设置的舞台保持其设置。
- **聚光灯阴影重新变得清晰**：修复了柔和阴影过度扩散而消失的问题（光栅、RT、PT 和离线 GI 通用）。
- **世界特效会接收舞台灯光（聚光灯 / 点光源）**：积雪和湿润地面会接收灯光及其阴影。
- **有专属舞台的歌曲也可以选择工作室地面**：选择"工作室（无舞台）"后会被保存。

### 📦 说明
- 可通过自动更新获取（仅下载更改的文件）。
