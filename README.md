# SHIELD Optimised Converter

**Codex was used to generate the code**

Convertisseur de vidéos MP4 directement sur 3DS, conçu pour produire des fichiers légers destinés à [SHIELD MP4 Player](https://github.com/Shield-77/Shield-MP4-Player). Le code a été généré avec l'aide de Codex et l'application vise aussi la Old 3DS.

**D'ici 5 jours shield mp4 converter recevra une refonte majeur et aura une autre utilité qui ne concernera que l'audio.**

**A été écrit le 24/11/2026 heure de paris**

## Version actuelle

La dernière release est **[v1.6.3](https://github.com/Shield-77/Shield-Optimised-MP4-Converter-/releases/tag/v1.6.3)** (`ShieldOptimisedConverter.cia`). Cette version ajoute le choix français/anglais au démarrage et retire la musique de fond pour alléger le CIA et la charge CPU. Les textes plus anciens qui annoncent cinq pistes intégrées ne décrivent plus la version actuelle.

Le convertisseur accepte certains MP4 H.264 jusqu'à 480p, avec AAC compatible ou sans audio. Il crée une vidéo de **256 × 144 au maximum**, à **15 ou 24 images/s** selon le choix de l'utilisateur ; l'AAC est recopié sans réencodage. La conversion logicielle peut être lente sur Old 3DS. Tester d'abord avec un court fichier et garder de l'espace libre sur la SD.

## Commandes

- **Haut/Bas** choisit un fichier ou dossier ; **Gauche/Droite** change de page ; **A** ouvre ou sélectionne ; **B** revient.
- Avant la conversion : **A** choisit 15 images/s, **X** choisit 24 images/s.
- **Y** lance un lot pour les MP4 détectés sur la SD. **START** annule et quitte.
- Les sorties terminées sont placées dans `sdmc:/SHIELD_MP4/` avec un nom distinct. Les fichiers sources ne sont pas modifiés.

## Source et dépendances

Le code de v1.6.3 est dans [`source/`](source/) et [`include/`](include/). Le code inutilisé de l'ancienne playlist et ses fichiers audio ne sont pas redistribués. La bannière du source public emploie un son silencieux. **Le CIA de la release reste inchangé.**

Pour compiler, installer devkitPro/devkitARM et fournir une version 3DS de **FFmpeg** avec ses en-têtes et bibliothèques ; indiquer son chemin avec `FFMPEG_ROOT`. Obtenir aussi `makerom` et `bannertool` séparément (`MAKEROM`, `BANNERTOOL`). Le présent dépôt ne fournit pas les bibliothèques FFmpeg précompilées de l'ancien environnement : leurs options de compilation et le code source exact doivent être documentés avant redistribution. La compilation du CIA publié n'est donc pas reproductible à partir de ce seul dépôt.

Le code original du convertisseur est sous [GPLv3](LICENSE). `minih264` et `minimp4` sont sous CC0, avec leurs avis dans [`include/`](include/). FFmpeg conserve sa licence propre, qui dépend des options de compilation ; voir [sa page officielle](https://ffmpeg.org/legal.html) et [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Le nom et les logos SHIELD restent l'identité de Shield-77 ; une adaptation doit être clairement indiquée.
