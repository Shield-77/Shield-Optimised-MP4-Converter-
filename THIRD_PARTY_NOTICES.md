# Avis sur les composants tiers

- `minih264e.h` : lieff/minih264, CC0 ; texte dans `include/MINIH264-LICENSE`.
- `minimp4.h` : lieff/minimp4, CC0 ; texte dans `include/MINIMP4-LICENSE`.
- FFmpeg : en-têtes et bibliothèques requis pour construire le lecteur et le convertisseur, non inclus dans ce dépôt. Selon la configuration, FFmpeg relève de LGPL ou GPL ; consulter https://ffmpeg.org/legal.html et conserver la configuration et les sources exactes de toute bibliothèque distribuée.
- `makerom` et `bannertool` : outils de paquetage à obtenir séparément.

La licence GPL du code SHIELD n'étend pas ses droits aux marques, contenus vidéo ou dépendances externes.

## Vérification des archives locales FFmpeg

Le dossier de travail local de ce projet contient les mêmes six archives FFmpeg, octet pour octet, que ceux de MP3 Downloader et de l’autre projet MP4. Les empreintes et la version source revendiquée par le fichier de build hérité figurent dans [l’audit MP3 Downloader](https://github.com/Shield-77/Shield-MP3-downloader/blob/main/LICENSING_STATUS.md). Cette égalité des archives **ne démontre pas** leur présence exacte dans les CIA de release ni la correspondance du commit source indiqué. Il faut conserver les journaux de compilation et produire les sources, modifications et options réellement utilisées avant de certifier la conformité des binaires.
