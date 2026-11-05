//---------------------------------------------------------------------------
// VehicleDb.cpp -- Base de vehicules AMS2 integree dans le binaire (Layer 1).
//
// Les donnees ne sont plus un JSON embarque : elles vivent dans des tableaux C++
// lisibles, un par fichier (VehicleDbData_A_F/_F_M/_M_S/_S_V.cpp), genere par :
//     python scripts/json_to_vehicle_db.py   (a partir de data/reference/ams2_vehicles_db.json)
// Ce fichier ne fait que parcourir le manifeste VehicleDbBuckets.h et exposer les
// donnees via l'API typree de VehicleDb.h. Plus aucun parse au demarrage : la base
// est pre-construite dans le binaire, ce qui rend chaque voiture editable a la main.
//---------------------------------------------------------------------------

#include "VehicleDb.h"

#include <algorithm>         // std::sort
#include <cctype>            // tolower (recherche insensible a la casse)

// Manifeste des tableaux de donnees : declare les buckets et fournit kAms2Buckets[]
// (table agregate, linkage interne -> n'inclure ce header que dans CE fichier).
#include "VehicleDbBuckets.h"   // Ams2BucketRef, kAms2Buckets[], VehicleDbRow

namespace {

std::string toLowerCopy(std::string s) {
    for (std::size_t i = 0; i < s.size(); ++i)
        s[i] = static_cast<char>(static_cast<unsigned char>(tolower(s[i])));
    return s;
}
// Forme canonique pour la recherche : minuscules + '_' remplace par ' '.
// Les cles du DB utilisent des espaces ("Alpine A110 GT4 Evo") tandis que les
// noms recus (gfx.car_model en live, colonnes LD) utilisent souvent des
// tirets-bas. Normaliser les deux cotés rend find() insensible aux deux.
std::string normalizeName(std::string s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = static_cast<char>(static_cast<unsigned char>(tolower(s[i])));
        if (c == '_') c = ' ';
        s[i] = c;
    }
    return s;
}

// Convertit une ligne brute (VehicleDbRow, agregat positionnel) en VehicleInfo typé.
// Le générateur garantit que chaque ligne émise est complète : chassis présent avec
// masse + CG valides et un jeu de freins plausible (les voitures sans données sont
// exclues à l'émission). Les champs optionnels absents portent kVehicleNoValue (-1.f)
// -> leurs drapeaux has* restent false, ce qui préserve la sémantique "pas de trou".
void rowToCar(const VehicleDbRow& r, VehicleInfo& car) {
    // Le .cpp pad le nom à droite (espaces) pour aligner les colonnes : on retire cette marge.
    std::string name(r.name);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
        name.pop_back();

    car.name  = name;
    car.alias  = r.alias ? std::string(r.alias) : std::string();        // Alias_name du JSON
    car.valid = true;   // toute ligne émise est valide par construction du générateur

    ChassisPhysics& ch = car.chassis;
    ch.present     = true;
    ch.massKg      = r.massKg;        ch.hasMass     = true;
    if (r.cgHeightM != kVehicleNoValue) { ch.cgHeightM   = r.cgHeightM;  ch.hasCgHeight = true; }

    // CG latéral/longitudinal : le DB ne porte que la forme scalaire fixe (pas de plage).
    CgRangeData& cgRt = ch.cgRight;
    if (r.cgRightM != kVehicleNoValue) { cgRt.present       = true; cgRt.hasFixedValue = true; cgRt.fixedValueM  = r.cgRightM; }
    CgRangeData& cgRe = ch.cgRear;
    if (r.cgRearM  != kVehicleNoValue) { cgRe.present       = true; cgRe.hasFixedValue = true; cgRe.fixedValueM  = r.cgRearM; }

    // Jeu de freins commun aux 4 roues : le générateur a validé optMin/optMax/fadeLow/
    // fadeHigh (bande plausible) pour chaque voiture émise -> present + hasFadeTemps true.
    for (int w = Ams2VehicleDb::kWheelFl; w <= Ams2VehicleDb::kWheelRr; ++w) {
        WheelBrakeData& wb = ch.brakes[w];
        wb.present             = true;
        wb.optTempMinC         = r.brake.optMinC;
        wb.optTempMaxC         = r.brake.optMaxC;
        wb.fadeTorqueLossBelow  = r.brake.fadeLossBelow;   // -1.f si absent (aucun consommateur actif)
        wb.fadeTorqueLossAbove  = r.brake.fadeLossAbove;
        wb.hasFadeTemps       = true;                      // FadeRange présent pour toute voiture émise
        wb.fadeRangeBelowC    = r.brake.fadeLowC;
        wb.fadeRangeAboveC    = r.brake.fadeHighC;
    }

    EnginePhysics& e = car.engine;
    if (r.oilOptC    != kVehicleNoValue) { e.optOilTempC      = r.oilOptC;     e.hasOptOilTemp     = true; }
    if (r.oilWarnC   != kVehicleNoValue) { e.oilWarningTempC  = r.oilWarnC;    e.hasOilWarningTemp = true; }
    if (r.waterWarnC != kVehicleNoValue) { e.waterWarningTempC= r.waterWarnC;  e.hasWaterWarningTemp=true; }
    e.present = e.hasOptOilTemp || e.hasOilWarningTemp || e.hasWaterWarningTemp;
}

// Comparateur par nom (minuscules) pour le tri deterministe de m_vehicles.
struct NameLess {
    bool operator()(const VehicleInfo& a, const VehicleInfo& b) const {
        return toLowerCopy(a.name) < toLowerCopy(b.name);
    }
};

} // namespace

// ===============================
// Ams2VehicleDb -- implementation
// ===============================

Ams2VehicleDb& Ams2VehicleDb::instance() {
    static Ams2VehicleDb s_instance;   // C++11 : initialisation thread-safe garantie
    return s_instance;
}

std::size_t Ams2VehicleDb::vehicleCount() const {
    return m_vehicles.size();
}

void Ams2VehicleDb::allNames(std::vector<std::string>& out) const {
    out.clear();
    for (std::size_t i = 0; i < m_lowerIndex.size(); ++i)
        out.push_back(m_vehicles[i].name);   // copie explicite, pas de move (style projet)
}

bool Ams2VehicleDb::find(const std::string& name, VehicleInfo& car) const {
    if (!m_valid || m_lowerIndex.empty()) return false;

    // Recherche binaire sur l'index normalise (minuscules + '_' -> ' ') : la
    // correspondance est insensible a la casse ET au tiret-bas/espace.
    std::string norm = normalizeName(name);
    if (norm.empty()) return false;
    std::size_t lo = 0, hi = m_lowerIndex.size();
    while (lo < hi) {
        std::size_t mid = lo + (hi - lo) / 2;
        if (m_lowerIndex[mid] == norm) {
            car = m_vehicles[mid];   // copie explicite, pas de move (style projet)
            return true;
        }
        if (m_lowerIndex[mid] < norm) lo = mid + 1; else hi = mid;
    }

    return false;
}

Ams2VehicleDb::Ams2VehicleDb() : m_valid(false) {
    // Parcours du manifeste de buckets : chaque bucket est un tableau C++ pre-construit.
    for (std::size_t b = 0; b < sizeof(kAms2Buckets)/sizeof(Ams2BucketRef); ++b) {
        const Ams2BucketRef& ref = kAms2Buckets[b];
        for (std::size_t i = 0; i < ref.count; ++i) {
            VehicleInfo car;
            rowToCar(ref.rows[i], car);

            m_vehicles.push_back(car);                       // copie explicite (style projet)
        }
    }

    std::sort(m_vehicles.begin(), m_vehicles.end(), NameLess());

    for (std::size_t i = 0; i < m_vehicles.size(); ++i) {
        m_lowerIndex.push_back(normalizeName(m_vehicles[i].name)); // copie explicite
    }

    m_valid = !m_vehicles.empty();
}
