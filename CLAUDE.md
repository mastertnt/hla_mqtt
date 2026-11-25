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
- `src/rti1516e/MqttTransport.*` : wrapper Paho (présence retained, Last Will, reconnexion, effacement des
  présences offline périmées ; thread d'entretien).
- `src/rti1516e/LocalSettings.*` : analyse de `localSettingsDesignator` (`mqtt://hôte:1883;clientId=...;prefix=hla`).
- `src/rti1516e/generated/` : produit par `tools/gen_rti_sources.py` (exceptions et stubs). Pour implémenter
  un service : coder la méthode, ajouter son nom à `IMPLEMENTED` dans le script, puis relancer le script.
- `src/rti1516e/MqttFederation.cpp` : create/destroy/list/join/resign (protocole décrit en tête de fichier
  et dans le README) ; `MqttSupportServices.cpp` : noms/handles, dimensions, getTimeFactory, decode*.
- `src/rti1516e/MqttObjects.cpp` : déclarations et objets (jalon 3), protocole décrit en tête de fichier ;
  `Wire.h` : helpers de topics (`hex64`, `encodeTopicLevel`, `classTopicPath`) et format binaire.
- `src/rti1516e/MqttSync.cpp` (points de synchronisation) et `MqttTime.cpp` (gestion du temps, protocole
  en tête de fichier) ; `TimeValue.h` : temps comparables et bornes (ouvertes/fermées).
- `src/rti1516e/MqttAdvisories.cpp` : callbacks d'avis à partir des abonnements publiés (`F/decl/<fédéré>`) ;
  commutateurs initialisés par les `<switches>` du FOM.
- `src/rti1516e/MqttOwnership.cpp` : propriété (chapitre 7) ; enregistrements `F/owners/<instance>/<fédéré>`,
  messages `F/ownership/...`, revendication par marqueur ; protocole décrit en tête du fichier.
- `src/rti1516e/MqttObjectOptions.cpp` : ordre et transport des messages envoyés (8.23, 8.24, 6.23 à 6.30),
  `getUpdateRateValueForAttribute` ; `retract` est dans `MqttTime.cpp` (message `WireKind::Retract`).
- `src/rti1516e/MqttRegions.cpp` : DDM (régions, associations de mise à jour, abonnements avec régions,
  10.28 à 10.30) ; abonnement effectif = union abonnement sans région / avec régions
  (`recomputeSubscriptions`) ; régions de mise à jour publiées dans l'enregistrement de propriété ; filtrage
  par recouvrement à la réception (`subscriptionCovers`, `interactionCovers`, `Extents.h`).
- `src/rti1516e/Trace.cpp` : `HLA_MQTT_TRACE` (Windows), trace des exceptions HLA levées par la DLL.
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
- `tests/ddm/main.cpp` : `hla-ddm-test` (broker requis), services DDM 9.2 à 9.11 et 10.28 à 10.30.
- `tests/mom/main.cpp` : `hla-mom-test` (broker requis), MOM (`src/rti1516e/MqttMom.cpp`).
- `tests/ownership/main.cpp` : `hla-ownership-test` (broker requis), propriété (`MqttOwnership.cpp`).
- `tests/robustness/main.cpp` : `hla-robustness-test` (broker requis, lié aussi à Paho : client brut qui
  reprend un clientId ; se relance avec `--child` pour un fédéré tué sans resign).
- `mim/HLAstandardMIM.xml` : MIM standard intégré à la DLL (tableau d'octets généré par `cmake/EmbedFile.cmake`).

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
  MOM ajouté depuis (issues #34 à #37).
- **Jalon 3 fait** (2026-11-23) : déclarations, objets, interactions, réservation de noms, requêtes de mise
  à jour, commutateurs d'avis mémorisés (sans callbacks). `hla-objects-test` 44/44 en Release et Debug
  (4 exécutions), ~2000 mises à jour/s mesurées dans un test mono-thread (borne basse). Broker propre après tests.
  Après le join, le fédéré n'est plus abonné à `F/#` mais à ses topics propres + couverture des classes.
  Propriété transférable depuis (issues #13 à #20).
- **Jalon 4 fait** (2026-11-23) : points de synchronisation, gestion du temps (régulation, contrainte,
  TAR/TARA/NER/NERA/FQR, TSO, GALT/LITS, lookahead, livraison asynchrone). `hla-time-test` 43/43 en Release
  et Debug (+3 exécutions) ; tous les tests précédents verts ; broker propre. Boucles 2 fédérés :
  ~160 pas/s (TAR, lookahead 1), ~140 pas/s (NER, lookahead 0). retract et change*OrderType faits depuis (issues #7, #8).
- **DDM, issues #21 à #24** (2026-11-24) : régions, bornes (10.28 à 10.30), enregistrement avec régions,
  (dés)association, abonnements objets/interactions avec régions. `hla-ddm-test` 51/51 en Release, suite
  complète verte.
- **DDM, issues #25 à #27** (2026-11-25) : envoi et requête avec régions, filtrage par recouvrement, avis de
  portée et de pertinence avec régions. `hla-ddm-test` 70/70 (3 exécutions), suite complète verte.
  Découverte non filtrée ; `conveyRegionDesignatorSets` non géré.
- **MOM, issues #34 à #37** (2026-11-25) : MIM standard fusionné à la création, objets HLAfederate (un par
  fédéré, valeurs calculées) et HLAfederation (locale), HLArequest/HLAreport, HLAadjust, HLAservice (file
  interne hors portée de callback), HLAreportFederateLost. `hla-mom-test` 50/50 (4 exécutions), suite verte.
  Rapport d'invocation limité aux services MOM ; HLAmodifyAttributeState et sauvegarde répondent
  HLAreportMOMexception.
- **Robustesse, issues #38 et #3** (2026-11-25) : reconnexion automatique (`reconnect`, défaut 30 s ; session
  propre, réabonnement + barrière, resynchronisation de la vue, republication des info d'objets), effacement
  des présences offline après `staleClientDelay`. `hla-robustness-test` 13/13, suite verte.
- **Propriété, issues #13 à #20** (2026-11-25) : plusieurs propriétaires par instance, un enregistrement
  retained par propriétaire (`F/owners/<instance>/<fédéré>`), `info` immuable ; messages JSON
  `F/ownership/<fédéré>` et `.../all` ; revendication des attributs sans propriétaire par marqueur
  (barrière). Tous les services et callbacks du chapitre 7, resign complet (`MqttOwnership.cpp`).
  `hla-ownership-test` 63/63 (9 exécutions, Release et Debug), suite complète verte (9/9).
- **Issues #7 à #12** (2026-11-25) : retract/requestRetraction, change*OrderType, changement et requête de
  transport, getUpdateRateValueForAttribute. `hla-time-test` 63/63, `hla-objects-test` 72/72 en Release.

## Essai VR-Link 5.4 (C:\MAKrlink5.4-netUtilitiesin64)
- f18HLA1516e_64 + netdumpHLA1516e_64 fonctionnent sur notre RTI (découverte, 45 attributs RPR reçus),
  sans `--ignoreAdvisories` depuis les avis (2026-11-24).
- DLL sous les noms MÄK : build séparé `build-mak` avec `-DHLA_MQTT_MAK_NAMES=ON` (noms + version PE 14.00,
  issues #4 et #5), dossier ajouté au PATH (ne rien copier dans C:\MAK). Artefact CI `...-windows-x64-mak`.
- VR-Link contrôle la version d'éditeur de liens PE de la DLL (14.00 attendu) : sinon f18 s'arrête,
  netdump plante. Écrite après l'édition de liens par `tools/pe_linker_version.cpp` (`hla-pe-linker-version`).
- Les processus tués laissent des topics retained (fédération) : nettoyer le broker. Les présences offline
  sont effacées automatiquement depuis #3.

## CI
- `.github/workflows/ci.yml` : Windows (VS2022 v143, Release + Debug) et Linux (GCC 13), Conan 2, presets
  `conan-*`, Mosquitto pour les tests (`broker/mosquitto-hla.conf`). Dépôt : github.com/mastertnt/hla_mqtt (public).

## Règles de concurrence
- Transport : le thread Paho prend `houseMutex` (présence, perte de connexion) ; ne jamais appeler Paho ni
  l'outbox en tenant `houseMutex` (interblocage observé avec `client.is_connected()`).
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
- `Octet` est un `char` signé : convertir en `unsigned char` avant de recomposer un entier (handles MOM).
- `for (x : Json::parse(s)["k"].items())` lit un temporaire détruit : garder le `Json` dans une variable.
- CMake : `set(X ${Y})` avec `Y` vide désaffecte `X` ; un `if(X STREQUAL "")` compare alors le nom littéral.
  Toujours `set(X "${Y}")` et `if("${X}" ...)`.
- Paho C : une connexion qui suit de près une déconnexion (même avec un autre handle) attend ~1 s. D'où la
  session propre plutôt qu'une session persistante purgée par une connexion supplémentaire.

## Suite prévue
1. `check_imports.py` sur un vrai fédéré (selftest Windows validé).
2. Compiler et tester l'encodage sous Linux/GCC.
3. Tester la substitution de DLL sur un vrai fédéré dès qu'un exécutable est disponible.
4. Sauvegarde/restauration.
