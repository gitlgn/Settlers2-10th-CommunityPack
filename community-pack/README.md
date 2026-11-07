# Settlers II – 10th Anniversary: Community Pack

*Die Siedler II – Die nächste Generation* · [Deutsch](#deutsch) · [English](#english)

An add-on for the [Community Patch](https://github.com/S2-modders/Settlers-DNG-Patches) by zocker_160 / S2-modders:
HUD scaling for high resolutions, sharper interface, soft and sharp shadows, HD textures, better post-processing.
**No game files are included** – the installer builds everything from your own copy of the game.

---

## Deutsch

### Was ist drin?

| Funktion | Schalter in `bin\d3d9.ini` | Beschreibung |
|---|---|---|
| UI-Skalierung | `[UI] Scale = 1.5` | Oberfläche, Fenster, Schrift und Minimap werden bei hohen Auflösungen vergrößert (z. B. 1.25 / 1.5 / 2.0). Maus und Klicks passen automatisch. Bei kleinen Auflösungen wird automatisch weniger vergrößert (nie unter 640×480). |
| UI-Filter | `[UI] Filter = linear` | `linear` = glatt, `point` = scharfe Pixel (gut bei `Scale = 2.0`). |
| HD-Oberfläche | `[Mods] HDInterface` | Alle Oberflächen-Texturen, das Menü-Logo und die Schriften 2× mit Real-ESRGAN hochgerechnet (wird beim Installieren auf deinem PC erzeugt). |
| HD-Texturen | `[Mods] HDTextures` | 4K-Texturen der Welt aus [S2-modders/Settlers-II-remakes-4k-textures-dds](https://github.com/S2-modders/Settlers-II-remakes-4k-textures-dds) (Download durch den Installer). |
| Weiche Schatten | `[Mods] SmoothShadows` | 3×3-PCF-Filter statt harter Treppenkanten, in allen Shadern inkl. Boden. |
| Genaue Schatten | `[Mods] SharpShadows` | 32-Bit- statt 16-Bit-Schattenkarte (weniger Artefakte). |
| Schattenauflösung | `[Graphics] ShadowMapScale = 2` | Schattenkarte 4096×4096 statt 2048×2048 (bei höchster Schattenstufe; `4` = 8192²). |
| Spiegelungen | `[Graphics] ReflectionMapScale = 2` | Wasserspiegelung 4096×2048 statt 2048×1024. |
| Sichtweite | `[Mods] LODFix` | Weniger Aufpoppen beim Herauszoomen (Werte aus dem 4K-Projekt). |
| Nachbearbeitung | `[Mods] ImprovedPostFX` | Gauß-Weichzeichner für Bloom, sanftere Tiefenunschärfe. |

Jede Funktion lässt sich einzeln abschalten (`off` bzw. `1`/`1.0`); nach dem nächsten Spielstart sind die Originaldateien automatisch wieder aktiv.

### Was du von einem Vanilla-Spiel aus brauchst

1. **Die Siedler II – Die nächste Generation / The Settlers II – 10th Anniversary**, am besten die **GOG-Version** (Build 11757).
   - Retail-/Gold-Edition: auf **Patch 11757** aktualisieren. Die Daten-Pakete funktionieren dann, UI-Skalierung und Schatten-/Spiegelungsauflösung sind aber nur für die GOG-Exe (SHA-256 `484a8afc…548b7`) freigeschaltet und schalten sich sonst selbst ab.
2. **Community Patch 2.3** von S2-modders installieren: [Settlers-DNG-Patches → Releases](https://github.com/S2-modders/Settlers-DNG-Patches/releases) (`CommunityPatch_2.3.zip`).
   Er bringt Widescreen, Zoom, Vulkan/DXVK (`d3d9vk.dll`) und die Mehrspieler-Lobby mit – dieses Pack baut darauf auf und ersetzt nur dessen `d3d9.dll` durch eine erweiterte Fassung.
3. Eine Grafikkarte mit **Shader Model 3** (praktisch jede GPU seit 2006) und Vulkan-Treiber.
4. Für die optionalen Teile: Internet (HD-Texturen ~800 MB, Upscaler ~45 MB).

Weitere Projekte der Community, die du kennen solltest:
- [S2-modders/Settlers-DNG-lobby-emulator](https://github.com/S2-modders/Settlers-DNG-lobby-emulator) – eigener Lobby-Server für den Online-Modus
- [S2-modders/S2-10th-Anniversary-decrypter](https://github.com/S2-modders/S2-10th-Anniversary-decrypter) – Spieldateien ent-/verschlüsseln (für Modder; liegt dem Pack bei)
- [S2-modders/s2-map-format](https://github.com/S2-modders/s2-map-format) – Dokumentation des Kartenformats
- [S2-modders/Settlers-II-remakes-4k-textures](https://github.com/S2-modders/Settlers-II-remakes-4k-textures) – Quellen der 4K-Texturen

### Installation

1. Community Patch 2.3 installieren (siehe oben) und das Spiel einmal starten.
2. Den Ordner `CommunityPack` in den Spielordner entpacken (neben `bin` und `data`).
3. `CommunityPack\install.cmd` ausführen. Der Installer
   - sichert die bisherige `d3d9.dll` als `d3d9.dll.communitypatch` und installiert die neue,
   - ergänzt `bin\d3d9.ini` um die Abschnitte `[Mods]`, `[UI]`, `[Graphics]`,
   - entschlüsselt die nötigen Dateien **deines** Spiels, ändert sie und legt sie unter `mods\` ab,
   - fragt, ob HD-Texturen geladen und die Oberfläche hochgerechnet werden soll.
4. Spiel starten. Beim ersten Start werden die Pakete eingespielt (einmalig ein paar Sekunden).

Deinstallation: `CommunityPack\uninstall.cmd` (stellt alle Originaldateien und die vorherige `d3d9.dll` wieder her, `-Purge` löscht zusätzlich `mods\`).

### Hinweise

- **Mehrspieler:** Die Pakete ändern nur Grafik und Oberfläche. Gemischte Installationen (mit/ohne Pack) sind nicht getestet – am besten installieren alle Mitspieler dieselben Pakete.
- Die Originaldateien liegen unter `mods\_backup` – nicht löschen, solange Pakete aktiv sind.
- Fehlersuche: `[Misc] DebugMode = on` schreibt Protokolle nach `bin\` (`modsync.log`, `uiscale.log`, `gfxpatches.log`, `crashdiag.log`).

---

## English

### Features

| Feature | Switch in `bin\d3d9.ini` | |
|---|---|---|
| HUD scaling | `[UI] Scale = 1.5` | Bigger HUD, dialogs, fonts and minimap on high resolutions; mouse input matches automatically. Reduced automatically on small resolutions (never below 640×480). |
| HUD filter | `[UI] Filter = linear` | `linear` smooth, `point` crisp pixels (best with `Scale = 2.0`). |
| HD interface | `[Mods] HDInterface` | All interface textures, the menu logo and the fonts upscaled 2× with Real-ESRGAN (generated on your PC). |
| HD textures | `[Mods] HDTextures` | 4K world textures from [S2-modders/Settlers-II-remakes-4k-textures-dds](https://github.com/S2-modders/Settlers-II-remakes-4k-textures-dds) (downloaded by the installer). |
| Soft shadows | `[Mods] SmoothShadows` | 3×3 PCF instead of jagged shadow edges, in every shader incl. terrain. |
| Precise shadows | `[Mods] SharpShadows` | 32-bit instead of 16-bit shadow map. |
| Shadow resolution | `[Graphics] ShadowMapScale = 2` | 4096² instead of 2048² at the highest shadow setting (`4` = 8192²). |
| Reflections | `[Graphics] ReflectionMapScale = 2` | Water reflection 4096×2048 instead of 2048×1024. |
| View distance | `[Mods] LODFix` | Less pop-in when zoomed out. |
| Post-processing | `[Mods] ImprovedPostFX` | Gaussian bloom blur, softer depth of field. |

### Requirements (starting from a vanilla game)

1. *The Settlers II – 10th Anniversary*, preferably the **GOG version** (build 11757). Retail/Gold: update to patch 11757; data packs work, HUD scaling and shadow/reflection resolution are GOG-only and switch themselves off otherwise.
2. **Community Patch 2.3**: [S2-modders/Settlers-DNG-Patches releases](https://github.com/S2-modders/Settlers-DNG-Patches/releases) – widescreen, zoom, Vulkan/DXVK and the multiplayer lobby. This pack builds on it and only replaces its `d3d9.dll` with an extended build.
3. A Shader Model 3 GPU with a Vulkan driver.

See the German section for related community projects (lobby emulator, decrypter, map format, 4K texture sources).

### Install

Install the Community Patch 2.3, extract `CommunityPack` into the game folder (next to `bin` and `data`), run `CommunityPack\install.cmd`, start the game. Uninstall with `uninstall.cmd`.

---

## Credits & licences

- Patch DLL: fork of [Settlers-DNG-Patches](https://github.com/S2-modders/Settlers-DNG-Patches) by **zocker_160** / S2-modders – **GPL-3.0**. Source of this build: this repository.
- See [THIRD-PARTY.md](THIRD-PARTY.md) for all other components.
- *The Settlers*, *Die Siedler* and *Blue Byte* are trademarks of Ubisoft Entertainment. This is an unofficial fan project, not affiliated with Ubisoft, Blue Byte or Funatics. No game files are distributed; you need your own copy of the game.
