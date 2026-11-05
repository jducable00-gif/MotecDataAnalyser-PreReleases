//---------------------------------------------------------------------------
// VehicleDbRow.h -- Ligne brute de la base vehicules AMS2 (Layer 1).
//
// Les donnees vivent dans des tableaux C++ lisibles, un par fichier :
//     VehicleDbData_A_F.cpp / _F_M / _M_S / _S_V   (+ manifeste VehicleDbBuckets.h)
// genere par scripts/json_to_vehicle_db.py a partir de data/reference/ams2_vehicles_db.json.
// Modifier une valeur = editer la ligne correspondante dans le .cpp, puis recompiler :
// plus aucun JSON embarque ni parse au demarrage (voir VehicleDb.cpp).
//
// IMPORTANT : BrakeSet et VehicleDbRow sont des AGREGATS purs -- AUCUN constructeur,
// meme pas un constructeur vide (un constructeur "fourni" exclurait la structure de
// l'initialisation par agregat dans tous les standards C++). Les tableaux de donnees
// s'initialisent donc positionnellement : { nom, masse, ..., {freins}, ... }.
// Ne JAMAIS y ajouter de constructeur ni d'init en classe.
//---------------------------------------------------------------------------

#pragma once

#include <cstddef>                    // std::size_t (compte de lignes par bucket)

const float kVehicleNoValue = -1.f;   // valeur "absente" pour les champs optionnels (CG / temperatures moteur)

struct BrakeSet {
    float optMinC, optMaxC;              // OptimumTempRange [min,max] (deg C), partage par FL/FR/RL/RR
    float fadeLossBelow, fadeLossAbove;  // FadeTorqueLoss Below/Above (-..+) ; -1.f si absent
    float fadeLowC, fadeHighC;           // FadeRange Below/Above (deg C)
};

struct VehicleDbRow {
    const char* name;                    // nom du vehicule (avec espaces) ; les .cpp le paddent d'espaces -> trimmer au chargement
    const char* alias;                   // Alias_name du JSON (ex "Brabham BT26A") ; nom d'affichage alternatif
    float massKg, cgHeightM, cgRightM, cgRearM;   // -1.f si absent (CG) / masse quasi toujours presente
    BrakeSet brake;                      // jeu de freins commun aux 4 roues
    float oilOptC, oilWarnC, waterWarnC;          // temperatures moteur ; -1.f si absentes
};
