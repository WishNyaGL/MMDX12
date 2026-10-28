# MMDX12 3.1.2 — Clearer RT Mirrors, Blush AO and Short Video Fixes

> 🌐 **Language:** [한국어](#한국어) · [English](#english) · [日本語](#日本語) · [简体中文](#简体中文)

## 한국어
### 🐞 수정 및 개선
- **레이 트레이싱에서 거울이 선명하게 비쳐요**: 거울 재질이 낮은 해상도의 반사를 회색 바탕에 반쯤 섞어 뿌옇게 보였어요. 이제 거울은 전체 해상도로 따로 추적한 반사를 그대로 보여 주고, 거울끼리 마주 보면 최대 3번까지 서로 비쳐요. 반사 속 물체도 해만이 아니라 스포트·포인트 조명과 그 그림자를 받아요.
- **볼터치처럼 반투명한 얼굴 레이어 아래로 AO가 끊기던 문제를 고쳤어요**: 볼터치·볼 선·눈물 같은 반투명 레이어가 있는 자리만 앰비언트 오클루전이 빠져서 볼에 밝은 사각형이 보였어요. 이제 캐릭터의 반투명 레이어는 그 아래 표면의 AO를 그대로 이어받아요(스테이지의 커튼처럼 얇은 천은 이전처럼 AO를 받지 않아요).
- **아주 짧은 영상을 내보내면 파일이 지워지던 문제를 고쳤어요**: 몇 프레임짜리 영상은 인코딩이 끝나기 전에 저장된 프레임 수를 세서 0으로 판단하고, 방금 만든 MP4를 지운 뒤 "렌더 취소"로 안내했어요. 이제 인코딩이 끝난 뒤에 세서 파일이 그대로 남아요.

### 🙏 기여
- AO와 짧은 영상 수정은 quasiquiescent 님의 기여(PR #6, #7)예요. 감사합니다!

### 📦 안내
- 자동 업데이트로 받을 수 있어요(변경된 파일만 내려받아요).

## English
### 🐞 Fixes and improvements
- **Mirrors are clear in ray tracing**: a mirror material blended a low-resolution reflection about halfway over its grey tint, so it looked milky. Mirrors now show a reflection traced separately at full resolution, and facing mirrors reflect each other up to 3 times. Objects seen in a reflection are now lit by spot and point lights with their shadows, not by the sun alone.
- **Ambient occlusion no longer breaks under translucent face layers such as blush**: wherever a translucent layer (blush, cheek lines, tears) covered the face, ambient occlusion was dropped, leaving pale rectangles on the cheeks. A character's translucent layers now keep the AO of the surface below them (sheer stage fabric such as curtains still takes no AO, as before).
- **Very short video exports no longer delete their file**: for a video of only a few frames, the saved frame count was read before encoding had finished, read as 0, and the MP4 that had just been written was removed with a "render cancelled" message. The count is now read after encoding finishes and the file stays.

### 🙏 Thanks
- The AO and short-video fixes were contributed by quasiquiescent (PR #6, #7). Thank you!

### 📦 Notes
- Available through the automatic updater (only changed files are downloaded).

## 日本語
### 🐞 修正と改善
- **レイトレーシングで鏡がくっきり映るようになりました**: 鏡の材質が低解像度の反射を灰色の下地に半分ほど混ぜていたため、白っぽくぼやけていました。鏡はフル解像度で別に追跡した反射をそのまま表示し、向かい合う鏡は最大 3 回まで映り合います。反射の中の物体も太陽だけでなく、スポット・ポイントライトとその影を受けるようになりました。
- **チークのような半透明の顔レイヤーの下で AO が途切れる問題を修正しました**: チーク・頬の線・涙などの半透明レイヤーがある部分だけアンビエントオクルージョンが抜け、頬に明るい四角形が見えていました。キャラクターの半透明レイヤーは下の面の AO をそのまま受け継ぐようになりました（ステージのカーテンのような薄い布は以前どおり AO を受けません）。
- **とても短い動画を書き出すとファイルが消える問題を修正しました**: 数フレームの動画では、エンコードが終わる前に保存フレーム数を数えて 0 と判断し、書き出したばかりの MP4 を削除して「レンダーをキャンセル」と表示していました。エンコード完了後に数えるようになり、ファイルが残ります。

### 🙏 貢献
- AO と短い動画の修正は quasiquiescent さんの貢献（PR #6、#7）です。ありがとうございます！

### 📦 お知らせ
- 自動アップデートで入手できます（変更されたファイルのみダウンロード）。

## 简体中文
### 🐞 修复与改进
- **光线追踪中的镜子变得清晰了**：镜面材质会把低分辨率的反射以一半左右的比例混在灰色底色上，看起来发白模糊。现在镜子直接显示以全分辨率单独追踪的反射，相对的镜子之间最多互相映照 3 次。反射中的物体也不再只受太阳照亮，还会受到聚光灯、点光源及其阴影的影响。
- **修复了腮红等半透明面部图层下环境光遮蔽中断的问题**：腮红、脸颊线条、眼泪等半透明图层覆盖的地方会丢失环境光遮蔽，脸颊上出现发亮的方块。现在角色的半透明图层会沿用下方表面的 AO（舞台上窗帘之类的薄布料和以前一样不受 AO 影响）。
- **修复了导出极短视频时文件被删除的问题**：只有几帧的视频会在编码完成前统计已保存的帧数，结果为 0，于是删除刚写好的 MP4 并提示"渲染已取消"。现在在编码完成后统计，文件会保留下来。

### 🙏 贡献
- AO 和短视频的修复由 quasiquiescent 贡献（PR #6、#7）。谢谢！

### 📦 说明
- 可通过自动更新获取（只下载更改过的文件）。
