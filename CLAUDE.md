# hla-mqtt-rti — contexte pour Claude Code

Répondre en français, ton neutre, sans tutoiement ni vouvoiement.

## Objectif
DLL RTI **IEEE 1516e (HLA Evolved)** dont le transport passe par **MQTT**, **sans processus RTI central**.
Elle doit être **binairement compatible** : un fédéré existant, déjà compilé contre la DLL d'un éditeur,
doit pouvoir charger `rti1516e.dll` sans recompilation.

## Cible et outillage
- Windows x64, **Visual Studio 2019 (v142)**, C++17, CRT partagée (`/MD`, `/MDd` en Debug -> `rti1516ed.dll`).
- **Conan 1.66** (profils `profiles/conan1/`), CMake 4.x sur le poste. Build complet : `build-vs2019.bat`.
- Dépendance : `paho-mqtt-cpp/1.4.0` (+ `paho-mqtt-c/1.3.13`), liée **statiquement**, sans SSL.
- Broker de dev : Mosquitto, `broker/start-broker.bat`.

## Règles ABI (ne pas casser)
- Headers IEEE 1516.1-2010 dans `include/RTI/` : **ne pas modifier**, sauf `SpecificConfig.h`.
  `encoding/` et `time/` viennent de Portico (openlvc/portico, texte IEEE), adaptés comme les autres
  (`RTI_THROW`, `RTI_NOEXCEPT`, `RTI_UNIQUE_PTR`) ; déclarations et ordre inchangés.
- `Octet` = `char` (`Integer8`), `Integer64` = `_int64` sous Windows : imposé par `EncodingConfig.h` IEEE.
- `RTI_UNIQUE_PTR` = `std::auto_ptr` (le type de retour fait partie du nom décoré MSVC) ; `_HAS_AUTO_PTR_ETC=1`.
- Les handles et `VariableLengthData` n'ont qu'un pointeur `_impl` (disposition imposée par les headers).
- Toute classe `RTI_EXPORT` à membres inline (ex. `NullFederateAmbassador`) doit être compilée dans la DLL
  pour être exportée (inclusion dans `CommonTypes.cpp`).
- `tools/check_imports.py federe.exe rti1516e.dll` compare les imports d'un fédéré aux exports de la DLL.

## Organisation du code
- `src/rti1516e/MqttRTIambassador.*` : implémentation de `RTIambassador`.
- `src/rti1516e/MqttTransport.*` : wrapper Paho (présence retained, Last Will).
- `src/rti1516e/LocalSettings.*` : analyse de `localSettingsDesignator` (`mqtt://hôte:1883;clientId=...;prefix=hla`).
- `src/rti1516e/generated/` : produit par `tools/gen_rti_sources.py` (exceptions et stubs). Pour implémenter
  un service : coder la méthode, ajouter son nom à `IMPLEMENTED` dans le script, puis relancer le script.
- `src/rti1516e/MqttFederation.cpp` : create/destroy/list/join/resign (protocole décrit en tête de fichier
  et dans le README) ; `MqttSupportServices.cpp` : noms/handles, dimensions, getTimeFactory, decode*.
- `src/rti1516e/MqttObjects.cpp` : déclarations et objets (jalon 3), protocole décrit en tête de fichier ;
  `Wire.h` : helpers de topics (`hex64`, `encodeTopicLevel`, `classTopicPath`) et format binaire.
- `src/rti1516e/MqttSync.cpp` (points de synchronisation) et `MqttTime.cpp` (gestion du temps, protocole
  en tête de fichier) ; `TimeValue.h` : temps comparables et bornes (ouvertes/fermées).
- `src/rti1516e/Fdd.*` : FDD fusionné, handles = FNV-1a 64 du nom qualifié ; `Xml.*`, `Json.h` : parseurs maison
  (pas de dépendance ajoutée).
- `MqttTransport` : subscribe/unsubscribe, `barrier()` (aller-retour sur `<prefix>/sync/<clientId>`), messages
  reçus sur un thread Paho -> `MqttRTIambassador::onMessage` (miroir `_view` des topics retained).
- `src/rti1516e/encoding/` : types de base (macro sur des codecs) et types construits ; chaque élément
  s'aligne lui-même sur sa frontière d'octets dans `encodeInto`/`decodeFrom` (`EncodingHelpers.h`).
- `src/rti1516e/time/StandardLogicalTimes.cpp` : HLAinteger64/HLAfloat64 Time, Interval, TimeFactory
  et `HLAlogicalTimeFactoryFactory` (nom vide -> HLAfloat64Time).
- `src/fedtime1516e/` : `fedtime1516e.dll` (`fedtime1516ed.dll` en Debug), exporte seulement
  `LogicalTimeFactoryFactory` (`RTI_EXPORT_FEDTIME`) et délègue à `HLAlogicalTimeFactoryFactory`.
- `tests/connect_federate/main.cpp` : `hla-test-federate --selftest` (API standard uniquement).
- `tests/encoding/main.cpp` : `hla-encoding-test`, octets attendus de la norme 1516.2, sans broker.
- `tests/federation/` : `hla-federation-test` (broker requis), plusieurs ambassadors, FOM de test dans `foms/`.
- `tests/objects/main.cpp` : `hla-objects-test` (broker requis), même FOM `foms/base.xml`.
- `tests/time/main.cpp` : `hla-time-test` (broker requis), synchronisation et temps.

## État au 2026-11-23
- **Jalon 1 fait** : connect/disconnect, callbacks `HLA_EVOKED` et `HLA_IMMEDIATE`, `connectionLost`,
  enable/disableCallbacks. Les autres services lèvent `RTIinternalError("not implemented yet")`.
- Linux/GCC + Conan 1.66 : build OK, selftest 14/14.
- Windows/VS2019 : Release et Debug compilent et lient ; `NullFederateAmbassador` exporté (65 symboles
  dans chaque DLL) ; selftest 14/14 en Release et en Debug contre le Mosquitto local (2026-11-23).
- **Encodage et temps faits** (2026-11-23) : `RTI/encoding` complet et `RTI/time` (HLAinteger64Time,
  HLAfloat64Time) dans `rti1516e.dll`, plus `fedtime1516e.dll`. `hla-encoding-test` 82/82 en Release
  et Debug sous VS2019. **Pas encore compilé sous Linux/GCC.**
- **Jalon 2 fait** (2026-11-23) : gestion de fédération + services de support liés au FDD.
  `hla-federation-test` 57/57 en Release et Debug (6 exécutions de suite, courses comprises), selftest 14/14,
  aucun topic retained laissé sur le broker. **Pas encore compilé sous Linux/GCC.**
  Pas de MOM (seuls HLAobjectRoot avec HLAprivilegeToDeleteObject et HLAinteractionRoot sont prédéfinis).
- **Jalon 3 fait** (2026-11-23) : déclarations, objets, interactions, réservation de noms, requêtes de mise
  à jour, commutateurs d'avis mémorisés (sans callbacks). `hla-objects-test` 44/44 en Release et Debug
  (4 exécutions), ~2000 mises à jour/s mesurées dans un test mono-thread (borne basse). Broker propre après tests.
  Après le join, le fédéré n'est plus abonné à `F/#` mais à ses topics propres + couverture des classes.
  Pas de propriété transférable.
- **Jalon 4 fait** (2026-11-23) : points de synchronisation, gestion du temps (régulation, contrainte,
  TAR/TARA/NER/NERA/FQR, TSO, GALT/LITS, lookahead, livraison asynchrone). `hla-time-test` 43/43 en Release
  et Debug (+3 exécutions) ; tous les tests précédents verts ; broker propre. Boucles 2 fédérés :
  ~160 pas/s (TAR, lookahead 1), ~140 pas/s (NER, lookahead 0). Non faits : retract, change*OrderType.

## Règles de concurrence
- Le thread Paho ne prend que `_viewMutex`. Ne jamais appeler un service bloquant du transport (publish
  synchrone, subscribe, unsubscribe, barrier) en tenant `_viewMutex` : calculer l'état sous le verrou, agir après.
- `publishAsync` est sûr sous `_viewMutex` : toutes les publications passent par l'outbox de `MqttTransport`
  (une file, un thread). Appeler Paho directement sous `_viewMutex` a causé un interblocage (jalon 4).
- Ordre FIFO émetteur -> récepteur : la gestion du temps en dépend. Publier les messages TSO avant l'état
  temporel qui les couvre (même outbox, donc même ordre).
- Une opération exclusive ne considère « pris » qu'un enregistrement abouti (`state` = `joined`,
  enregistrement `execution`) : sinon tous les concurrents peuvent abandonner sans gagnant.

## Pièges rencontrés
- Le recipe ConanCenter Conan 1 de `paho-mqtt-cpp/1.4.0` n'a pas d'option `ssl` (seul `paho-mqtt-c` l'a).
- CMake 4 refuse `cmake_minimum_required` < 3.5 (paho-mqtt-c) : `CMAKE_POLICY_VERSION_MINIMUM=3.5`
  est défini dans les profils (`[env]`) et dans `build-vs2019.bat`.
- Avertissements C4251/C4275 : inhérents à l'API standard, neutralisés par `/wd4251 /wd4275`.

## Suite prévue
1. `check_imports.py` sur un vrai fédéré (selftest Windows validé).
2. Compiler et tester l'encodage sous Linux/GCC.
3. Tester la substitution de DLL sur un vrai fédéré dès qu'un exécutable est disponible.
4. Retract et change*OrderType, puis propriété, MOM, DDM, callbacks d'avis, sauvegarde/restauration.
5. Effacer la présence `offline` retained d'un client disparu (aujourd'hui elle reste sur le broker).
