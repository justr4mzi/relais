// Relais - interface strings (English and French)
// SPDX-License-Identifier: MIT
#pragma once

enum Language { LANG_EN = 0, LANG_FR = 1 };
extern int g_lang;

// X(id, English, French)
#define RELAIS_STRINGS(X) \
    X(STR_SUBTITLE, L"Your console in fullscreen, with as little delay as possible.", L"Ta console en plein écran, avec le moins de latence possible.") \
    X(STR_SEC_SOURCE, L"Source", L"Source") \
    X(STR_VIDEO, L"Video", L"Vidéo") \
    X(STR_FORMAT, L"Format", L"Format") \
    X(STR_SOUND, L"Sound", L"Son") \
    X(STR_SEC_PICTURE, L"Picture", L"Image") \
    X(STR_RANGE, L"Colour range", L"Plage de couleurs") \
    X(STR_MATRIX, L"Matrix", L"Matrice") \
    X(STR_BRIGHTNESS, L"Brightness", L"Luminosité") \
    X(STR_CONTRAST, L"Contrast", L"Contraste") \
    X(STR_SATURATION, L"Saturation", L"Saturation") \
    X(STR_SHARPNESS, L"Sharpness", L"Netteté") \
    X(STR_RESET, L"Reset picture", L"Réinitialiser l'image") \
    X(STR_SEC_DISPLAY, L"Display", L"Affichage") \
    X(STR_EXCLUSIVE, L"Exclusive fullscreen (needed for AFMF on RX 6000)", L"Plein écran exclusif (nécessaire pour AFMF sur RX 6000)") \
    X(STR_VSYNC, L"VSync (less tearing, a little more delay)", L"VSync (moins de déchirures, un peu plus de latence)") \
    X(STR_DUPLICATES, L"Skip duplicate frames (30 FPS games captured at 60)", L"Ignorer les images en double (jeux à 30 FPS capturés en 60)") \
    X(STR_THRESHOLD, L"Threshold", L"Seuil") \
    X(STR_HELP, L"Help", L"Aide") \
    X(STR_START, L"Start", L"Lancer") \
    X(STR_RANGE_AUTO, L"Auto", L"Auto") \
    X(STR_RANGE_LIMITED, L"Limited (16-235): if blacks look grey", L"Limitée (16-235) : si les noirs sont gris") \
    X(STR_RANGE_FULL, L"Full (0-255): if blacks are crushed", L"Pleine (0-255) : si les noirs sont écrasés") \
    X(STR_MATRIX_AUTO, L"Auto", L"Auto") \
    X(STR_MATRIX_601, L"BT.601 (SD, JPEG)", L"BT.601 (SD, JPEG)") \
    X(STR_MATRIX_709, L"BT.709 (HD)", L"BT.709 (HD)") \
    X(STR_NO_SOUND, L"None (no sound)", L"Aucun (pas de son)") \
    X(STR_NO_MODES, L"This card offers no video format. Another app may already be using it.", L"Cette carte ne propose aucun format vidéo. Elle est peut-être déjà utilisée par une autre appli.") \
    X(STR_NO_DEVICE, L"No capture card found. Plug it in, then click ↻.", L"Aucune carte de capture trouvée. Branche-la puis clique sur ↻.") \
    X(STR_READY, L"Ready. Press Esc to leave fullscreen.", L"Prêt. Échap pour quitter le plein écran.") \
    X(STR_PICK_FIRST, L"Choose a capture card and a format first.", L"Choisis d'abord une carte de capture et un format.") \
    X(STR_LAST_SESSION, L"Last session: %.1f frames/s received, %.1f shown", L"Dernière session : %.1f images/s reçues, %.1f affichées") \
    X(STR_DUPS_SKIPPED, L" (%llu duplicates skipped)", L" (%llu doublons ignorés)") \
    X(STR_HELP_TITLE, L"Relais - Help", L"Relais - Aide") \
    X(STR_HELP_TEXT, \
      L"Relais shows your capture card in fullscreen with as little delay as possible.\n\n" \
      L"In fullscreen:\n" \
      L"  Esc\tBack to this window\n" \
      L"  D\tSkip duplicate frames (on/off)\n" \
      L"  R\tNext colour range (Auto, Limited, Full)\n" \
      L"  M\tNext matrix (Auto, BT.601, BT.709)\n" \
      L"  + / -\tBrightness\n" \
      L"  V\tVSync (on/off)\n" \
      L"  Alt+Enter\tLeave / re-enter exclusive fullscreen\n\n" \
      L"AFMF (RX 6000): turn on Fluid Motion Frames in AMD Adrenalin and keep \"Exclusive fullscreen\" checked. " \
      L"For a 30 FPS game captured at 60, keep \"Skip duplicate frames\" checked.\n\n" \
      L"Washed-out colours (grey blacks): set the range to \"Limited\". Crushed blacks: \"Full\".", \
      L"Relais affiche ta carte de capture en plein écran avec le moins de latence possible.\n\n" \
      L"Pendant l'affichage :\n" \
      L"  Échap\tRevenir à cette fenêtre\n" \
      L"  D\tIgnorer les doublons (on/off)\n" \
      L"  R\tPlage de couleurs suivante (Auto, Limitée, Pleine)\n" \
      L"  M\tMatrice suivante (Auto, BT.601, BT.709)\n" \
      L"  + / -\tLuminosité\n" \
      L"  V\tVSync (on/off)\n" \
      L"  Alt+Entrée\tQuitter / revenir au plein écran exclusif\n\n" \
      L"AFMF (RX 6000) : active Fluid Motion Frames dans AMD Adrenalin et laisse « Plein écran exclusif » coché. " \
      L"Pour un jeu à 30 FPS capturé en 60, laisse « Ignorer les images en double » coché.\n\n" \
      L"Couleurs délavées (noirs gris) : mets la plage sur « Limitée ». Noirs écrasés : « Pleine ».") \
    X(STR_MF_MISSING, L"Media Foundation is not available. On Windows N editions, install the Media Feature Pack.", L"Media Foundation est indisponible. Sur Windows N, installe le Media Feature Pack.") \
    X(STR_LANG_PROMPT, L"Choose your language", L"Choisis ta langue") \
    X(STR_ERR_OPEN_CARD, L"Could not open the capture card (0x%08X). Another app (OBS, Discord...) may be using it.", L"Impossible d'ouvrir la carte de capture (0x%08X). Elle est peut-être utilisée par une autre appli (OBS, Discord...).") \
    X(STR_ERR_FORMAT_REFUSED, L"The card refused the chosen format (0x%08X).", L"Le format choisi n'est pas accepté par la carte (0x%08X).") \
    X(STR_ERR_READER, L"Could not create the video reader (0x%08X).", L"Impossible de créer le lecteur vidéo (0x%08X).") \
    X(STR_ERR_NO_OUTPUT, L"No usable output format for this stream (0x%08X).", L"Aucun format de sortie utilisable pour ce flux (0x%08X).") \
    X(STR_ERR_CAPTURE_STOPPED, L"Capture stopped (0x%08X). The card may have been unplugged.", L"La capture s'est arrêtée (0x%08X). La carte a peut-être été débranchée.") \
    X(STR_ERR_END_OF_STREAM, L"End of the video stream.", L"Fin du flux vidéo.") \
    X(STR_ERR_FORMAT_CHANGED, L"The stream format changed and can no longer be read.", L"Le format du flux a changé et n'est plus lisible.") \
    X(STR_ERR_NO_RESPONSE, L"The capture card is not responding.", L"La carte de capture ne répond pas.") \
    X(STR_ERR_DX, L"DirectX 11 could not start (0x%08X).", L"DirectX 11 n'a pas pu démarrer (0x%08X).") \
    X(STR_ERR_SWAPCHAIN, L"Could not create the swap chain (0x%08X).", L"Impossible de créer la chaîne d'affichage (0x%08X).") \
    X(STR_ERR_D3DCOMPILER, L"d3dcompiler_47.dll was not found (Windows 10 or 11 required).", L"d3dcompiler_47.dll est introuvable (Windows 10 ou 11 requis).") \
    X(STR_ERR_SHADER, L"Shader error: ", L"Erreur de shader : ") \
    X(STR_ERR_DX_INCOMPLETE, L"DirectX setup is incomplete.", L"Initialisation DirectX incomplète.") \
    X(STR_ERR_SOUND, L"Sound: ", L"Son : ") \
    X(STR_ERR_AUDIO_THREAD, L"Could not start the sound.", L"Impossible de démarrer le son.") \
    X(STR_ERR_AUDIO_START, L"The sound could not start.", L"Le son n'a pas pu démarrer.") \
    X(STR_ERR_AUDIO_OPEN, L"Could not open the audio input", L"Impossible d'ouvrir l'entrée audio") \
    X(STR_ERR_AUDIO_READ, L"Could not read the audio input", L"Impossible de lire l'entrée audio") \
    X(STR_ERR_AUDIO_FORMAT, L"Audio format not supported", L"Format audio non pris en charge") \
    X(STR_ERR_AUDIO_FORMAT2, L"Unreadable audio format", L"Format audio illisible") \
    X(STR_ERR_SPEAKERS, L"Could not open the speakers", L"Impossible d'ouvrir les haut-parleurs")

#define RELAIS_STRING_ID(id, en, fr) id,
enum StringId { RELAIS_STRINGS(RELAIS_STRING_ID) STR_COUNT };
#undef RELAIS_STRING_ID

const wchar_t* T(StringId id);
