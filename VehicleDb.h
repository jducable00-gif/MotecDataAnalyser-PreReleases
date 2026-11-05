//---------------------------------------------------------------------------
// VehicleDb.h -- Base de vehicules AMS2 integree dans le binaire (Layer 1).
// ATTENTION : cette base ne couvre QUE les vehicules d'Assetto Corsa Mod 2.
// AC EVO et PMR n'y sont pas representes ; pour ces sims, les donnees theoriques
// doivent provenir d'une autre source (ou rester absentes -> has* == false).
//
// Les donnees vivent dans des tableaux C++ lisibles (VehicleDbData_*.cpp), genere par
// scripts/json_to_vehicle_db.py a partir de data/reference/ams2_vehicles_db.json, et sont
// pre-construites au chargement du binaire (plus aucun JSON embarque ni parse). L'application ne
// lit donc plus aucun fichier externe pour les donnees theoriques des vehicules :
// plages min/max, pas, valeurs par defaut, masses/CG, temperatures optimales...
// (voir .ai/skills/setup_analysis.md -- "The Three Layers of Truth", Layer 1).
//
// Usage:
//     const Ams2VehicleDb& db = Ams2VehicleDb::instance();
//     if (!db.isValid()) { ... }                       // JSON corrompu/absent
//     VehicleInfo car;
//     if (db.find("Alpine_A110_GT4_Evo", car))         // exact, puis insensible a la casse
//         use(car.chassis.massKg);                     // 1165.0
//---------------------------------------------------------------------------

#pragma once

#include <string>
#include <vector>

// ===============================
// Structures typrees de la base vehicules AMS2.
// Chaque champ porte un "present" : les donnees evoluent (nouvelles colonnes dans
// VehicleDbRow.h + re-generation), on ne veut pas d'undefined behavior sur les champs manquants.
// ===============================

struct WheelBrakeData {
    bool present = false;          // section "brakes.<FL|FR|RL|RR>" presente ?
    double optTempMinC = 0.0, optTempMaxC = 0.0;   // OptimumTempRange [min,max] (deg C)
    double fadeTorqueLossBelow = 0.0, fadeTorqueLossAbove = 0.0;
    bool hasFadeTemps = false;     // FadeRangeBelow/Above presents ?
    double fadeRangeBelowC = 0.0, fadeRangeAboveC = 0.0;

    WheelBrakeData() {}            // C++Builder: constructeur explicite vide requis
};

// Position du centre de gravite latéral/longitudinal (m). Deux formes dans le DB :
//  - scalaire JSON            -> valeur FIXE (isAdjustable() == false)
//  - objet {min, step, max}   -> plage REGLABLE en jeu ; la position courante est
//    min + step * <Setting> et exposee par valueM(). La presence d'une plage est
//    elle-meme une information exploitable (le pilote peut deplacer le CG).
struct CgRangeData {
    bool present = false;          // cle presente dans le JSON ?
    double fixedValueM  = 0.0, hasFixedValue = false;   // forme scalaire
    double minM         = 0.0, stepM = 0.0, maxPositions = 0.0;  // forme objet {min,step,max}
    double settingIndex = 0.0, hasSetting = false;      // CG*Setting (index de position)

    bool isAdjustable() const { return present && !hasFixedValue; }
    double valueM() const {      // position courante (m), dans les deux formes
        if (!present)            return 0.0;
        if (hasFixedValue)       return fixedValueM;
        return minM + stepM * settingIndex;   // = CG*Setting du DB (= valeur resolue)
    }

    CgRangeData() {}              // C++Builder: constructeur explicite vide requis
};

struct ChassisPhysics {
    bool present = false;          // section "physics.chassis" presente ?
    double massKg = 0.0, hasMass = true;      // Mass (kg) -- quasi toujours present
    double cgHeightM = 0.0, hasCgHeight = false;   // CGHeight (m)

    CgRangeData cgRight;           // CGRightRange : scalaire fixe OU plage reglable (+setting)
    CgRangeData cgRear;            // CGRearRange  : idem

    WheelBrakeData brakes[4];      // [FL, FR, RL, RR] -- index: kWheelFl=0..kWheelRr=3
};

struct EnginePhysics {
    bool present = false;          // section "physics.engine" presente ?
    double optOilTempC = 0.0, hasOptOilTemp = false;      // OptimumOilTemp (deg C)
    double oilWarningTempC = 0.0, hasOilWarningTemp = false;   // OilWarningTemp
    double waterWarningTempC = 0.0, hasWaterWarningTemp = false;// WaterWarningTemp

    EnginePhysics() {}             // C++Builder: constructeur explicite vide requis
};

struct VehicleInfo {
    std::string name;              // cle du DB (ex "Alpine A110 GT4 Evo")
    std::string alias;             // Alias_name du JSON (ex "Brabham BT26A") ; nom d'affichage alternatif
    bool valid = false;            // true si l'entree a ete trouvee + parsee
    ChassisPhysics chassis;
    EnginePhysics engine;

    VehicleInfo() {}               // C++Builder: constructeur explicite vide requis
};

// ===============================
// Ams2VehicleDb -- point d'accès unique à la base embarquée (singleton).
// La base est construite une seule fois, au premier appel, depuis les tableaux C++
// pre-construits de VehicleDbData_*.cpp. Si aucune entree n'est produite, isValid() == false
// et find()/vehicleCount() sont sûrs.
// ===============================

class Ams2VehicleDb {
public:
    static const int kWheelFl = 0;   // index dans ChassisPhysics::brakes[]
    static const int kWheelFr = 1;
    static const int kWheelRl = 2;
    static const int kWheelRr = 3;

    static Ams2VehicleDb& instance();

    bool isValid() const { return m_valid; }        // base construite au chargement du binaire ?
    std::size_t vehicleCount() const;               // nombre de véhicules (0 si invalide)

    // Noms des véhicules, triés par ordre lexicographique (déterministe).
    void allNames(std::vector<std::string>& out) const;

    // Recherche : insensible à la casse et au tiret-bas/espace (les noms live
    // gfx.car_model utilisent '_' tandis que les clés du DB utilisent ' ').
    // Retourne false si aucun véhicule ne correspond ; car est alors vide.
    bool find(const std::string& name, VehicleInfo& car) const;

private:
    Ams2VehicleDb();                 // construit la base depuis les tableaux C++ (une seule fois)
    ~Ams2VehicleDb() {}              // non copiable -- singleton (constructeur privé)

    Ams2VehicleDb(const Ams2VehicleDb&);            // interdits : pas de copie du DB
    Ams2VehicleDb& operator=(const Ams2VehicleDb&);

    bool m_valid;                    // true si la construction initiale a produit des entrees
    std::vector<VehicleInfo> m_vehicles;   // entrees valides, triees par nom (insensible casse)
    std::vector<std::string> m_lowerIndex;  // noms en minuscules, meme ordre que m_vehicles
};
