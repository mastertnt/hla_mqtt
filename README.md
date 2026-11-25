# hla-mqtt-rti

DLL **IEEE 1516e (HLA Evolved)** dont le transport passe par **MQTT**, sans processus RTI central.
Elle est conçue pour remplacer sans recompilation la DLL RTI chargée par un fédéré 1516e existant.

> Jalon 1 : `connect` / `disconnect`, livraison des callbacks (`HLA_EVOKED` et `HLA_IMMEDIATE`), `connectionLost`.
> Encodage (`RTI/encoding`) et temps logiques standard (`RTI/time`, `fedtime1516e.dll`).
> Jalon 2 : gestion de fédération (create/destroy/list/join/resign) et services de support liés au FDD
> (noms et handles des classes, attributs, interactions, paramètres, dimensions, fédérés).
> Jalon 3 : gestion des déclarations (publish/subscribe) et des objets (register/discover,
> update/reflect, interactions, delete/remove, réservation de noms, requêtes de mise à jour).
> Jalon 4 : points de synchronisation et gestion du temps (régulation, contrainte, TAR/TARA/NER/NERA/FQR,
> livraison TSO, GALT/LITS, lookahead, livraison asynchrone).
> Tous les autres services lèvent `RTIinternalError("... not implemented yet")`.

## Arborescence

```
include/RTI/            headers IEEE 1516.1-2010 (SpecificConfig.h adapté, voir plus bas)
include/RTI/encoding/   helpers d'encodage IEEE (HLAinteger32BE, HLAfixedRecord, ...)
include/RTI/time/       temps logiques standard (HLAinteger64Time, HLAfloat64Time)
src/rti1516e/
  CommonTypes.cpp       Exception, bases des ambassadors, RangeBounds, Typedefs, bases du temps logique
  Handles.cpp           les 9 classes de handles (uint64, encodage 8 octets big-endian)
  VariableLengthData.cpp
  LocalSettings.*       analyse du localSettingsDesignator
  MqttTransport.*       wrapper Paho MQTT C++ (présence, Last Will, reconnexion)
  MqttRTIambassador.*   implémentation de RTIambassador (connexion, callbacks)
  MqttFederation.cpp    gestion de fédération (protocole MQTT décrit plus bas)
  MqttSupportServices.cpp  services de support (noms, handles, dimensions, temps)
  MqttObjects.cpp       déclarations et objets ; Wire.h : format binaire des messages
  MqttSync.cpp          points de synchronisation
  MqttTime.cpp          gestion du temps ; TimeValue.h : temps comparables, bornes
  Fdd.*                 FDD fusionné, handles déterministes ; Xml.*, Json.h : parseurs minimaux
  RTIambassadorFactory.cpp
  encoding/             types de base et construits (IEEE 1516.2-2010, 4.13)
  time/                 HLAinteger64Time, HLAfloat64Time, fabriques, HLAlogicalTimeFactoryFactory
src/fedtime1516e/       fedtime1516e.dll : LogicalTimeFactoryFactory
  generated/            produit par tools/gen_rti_sources.py (exceptions et stubs des ~145 services)
tests/connect_federate/ fédéré de test (API standard uniquement)
tests/encoding/         hla-encoding-test : encodage et temps logiques, sans broker
tests/federation/       hla-federation-test : plusieurs ambassadors, modules FOM de test (foms/)
tests/objects/          hla-objects-test : publish/subscribe, objets, interactions entre fédérés
tests/time/             hla-time-test : points de synchronisation, gestion du temps
tests/ddm/              hla-ddm-test : régions, associations, abonnements avec régions
tests/mom/              hla-mom-test : MOM (objets HLAmanager, requêtes, rapports, services)
tests/robustness/       hla-robustness-test : reconnexion, présences périmées
mim/HLAstandardMIM.xml  MIM standard IEEE 1516.1-2010, intégré à la DLL (cmake/EmbedFile.cmake)
tools/
  gen_rti_sources.py    régénère generated/ à partir des headers
  check_imports.py      compare les imports d'un fédéré aux exports de la DLL (Windows)
  pe_linker_version.cpp hla-pe-linker-version : version d'éditeur de liens PE (VR-Link)
profiles/conan1/        profils Conan 1.66 (vs2019-release, vs2019-debug)
profiles/conan2/        profils Conan 2 (vs2019, linux-gcc)
broker/                 configuration et scripts Mosquitto
build-vs2019.bat        build Release + Debug (Conan 1.66)
```

## Compilation Windows, Visual Studio 2019, Conan 1.66

Prérequis : Conan 1.66, CMake ≥ 3.20 et un profil Conan `default` (`conan profile new default --detect`).

En une commande, Release et Debug :

```bat
build-vs2019.bat
```

Ce qui équivaut à :

```bat
conan install . -pr:h profiles\conan1\vs2019-release -pr:b default --build=missing
conan install . -pr:h profiles\conan1\vs2019-debug   -pr:b default --build=missing
cmake -S . -B build -G "Visual Studio 16 2019" -A x64 -DCMAKE_TOOLCHAIN_FILE=build/generators/conan_toolchain.cmake
cmake --build build --config Release
cmake --build build --config Debug
```

Sorties : `build/bin/Release/rti1516e.dll` et `fedtime1516e.dll`, `build/bin/Debug/rti1516ed.dll` et `fedtime1516ed.dll` (CRT `/MDd`, pour les fédérés Debug), les `.lib`, `hla-test-federate.exe` et `hla-encoding-test.exe`.
Le fichier `build/hla_mqtt_rti.sln` s'ouvre directement dans Visual Studio 2019.

Avec Conan 2 : `conan install . -pr:a profiles/conan2/vs2019 --build=missing`, puis la même étape CMake.

**CMake 4.x** : `paho-mqtt-c` 1.3.13 déclare `cmake_minimum_required` < 3.5, ce que CMake 4 refuse. Les profils et `build-vs2019.bat` définissent donc `CMAKE_POLICY_VERSION_MINIMUM=3.5`.

Paho est lié **statiquement** dans la DLL : un fédéré n'a besoin que de `rti1516e.dll`.
Sans accès à ConanCenter, `paho-mqtt-cpp/1.4.0` et `paho-mqtt-c` doivent être présents dans le remote Conan interne (Artifactory, par exemple).

## Broker MQTT

Le dossier `broker/` contient une configuration Mosquitto 2.x de développement (accès anonyme, sans TLS) :

```bat
broker\start-broker.bat     :: Mosquitto en avant-plan (MOSQUITTO_DIR si installé ailleurs)
broker\watch-hla.bat        :: affiche le trafic hla/#
```

Avec Docker : `docker compose -f broker/docker-compose.yml up`.

### Options

| Option Conan / CMake | Défaut | Rôle |
|---|---|---|
| `library_name` / `HLA_MQTT_RTI_LIBRARY_NAME` | `rti1516e` | Nom du fichier DLL, à aligner sur ce qu'importe le fédéré |
| `HLA_MQTT_FEDTIME_LIBRARY_NAME` | `fedtime1516e` | Nom du fichier DLL fedtime (`LogicalTimeFactoryFactory`) |
| `HLA_MQTT_RTI_DEBUG_POSTFIX` | `d` | Suffixe Debug |
| `mak_names` / `HLA_MQTT_MAK_NAMES` | `False` | Noms MÄK (`librti1516e64.dll`, `libfedtime1516e64.dll` en 64 bits, sans `64` en 32 bits) et version d'éditeur de liens PE 14.00 ; remplace les deux noms ci-dessus |
| `pe_linker_version` / `HLA_MQTT_PE_LINKER_VERSION` | vide | Windows : version d'éditeur de liens écrite dans l'en-tête PE des DLL après l'édition de liens (`14.00` pour VR-Link) ; vide : inchangée (14.00 avec `mak_names`) |
| `use_unique_ptr` / `HLA_MQTT_RTI_USE_UNIQUE_PTR` | `False` | `std::unique_ptr` au lieu de `std::auto_ptr` dans les signatures (ABI des headers OpenRTI en C++17 sous GCC/Clang) |
| `with_ssl` | `False` | Paho avec TLS (`ssl://`, `mqtts://`) |

Exemple (Conan 1) : `conan install . -pr:h profiles\conan1\vs2019-release -pr:b default -o library_name=rti1516e64 --build=missing`

DLL sous les noms MÄK : `-DHLA_MQTT_MAK_NAMES=ON` (CMake) ou `-o mak_names=True` (Conan). La CI publie aussi l'artefact `hla-mqtt-rti-windows-x64-mak` (Release). La version PE est écrite par l'outil `hla-pe-linker-version` (`tools/pe_linker_version.cpp`, compilé avec le projet), qui vérifie aussi un fichier : `hla-pe-linker-version --check librti1516e64.dll 14.00`.

## Compatibilité binaire : règles appliquées

1. **Headers IEEE d'origine** : disposition mémoire (un seul pointeur `_impl` par handle et par `VariableLengthData`), ordre des vtables et symboles identiques.
2. **`std::auto_ptr` imposé** : sous MSVC, le type de retour fait partie du nom décoré (`createRTIambassador`, `getTimeFactory`…). `SpecificConfig.h` fixe donc `RTI_UNIQUE_PTR = std::auto_ptr` quel que soit le standard C++, et le build définit `_HAS_AUTO_PTR_ETC=1` pour C++17.
3. **CRT partagée** : `/MD` en Release, `/MDd` en Debug. Toolsets v140 à v143 compatibles. La DLL compilée avec VS2019 nécessite un redistribuable VC++ 2019 ou plus récent.
4. **Linux** : même logique avec l'ABI libstdc++. Un fédéré compilé avec `_GLIBCXX_USE_CXX11_ABI=0` (anciens devtoolset RHEL) demande une DLL compilée de la même façon.

### Vérifier qu'un fédéré existant peut charger la DLL

```bat
dumpbin /dependents federe.exe            :: nom exact de la DLL RTI importée
python tools\check_imports.py federe.exe build\bin\Release\rti1516e.dll
```

Le script liste les symboles importés par le fédéré et absents de la DLL. La liste doit être vide pour un remplacement direct.
Les symboles d'**encodage** (`RTI/encoding`) et des temps standard (`RTI/time`) sont dans `rti1516e.dll` ; `LogicalTimeFactoryFactory` est dans `fedtime1516e.dll`. Pour un fédéré qui importe aussi la DLL fedtime, lancer le script une seconde fois avec `build\bin\Release\fedtime1516e.dll`.

### Diagnostic : HLA_MQTT_TRACE (Windows)

`HLA_MQTT_TRACE=1` (ou un chemin de fichier) trace chaque exception HLA levée par la DLL, avec son type et son message, même quand le fédéré l'attrape. Indispensable quand un fédéré existant abandonne sans dire pourquoi.

### Essai avec VR-Link 5.4 (MÄK)

Testé avec `f18HLA1516e_64.exe` (publie un F-18, FOM RPR) et `netdumpHLA1516e_64.exe` (affiche les entités et leurs mises à jour), sans les recompiler :

- `check_imports.py` : 0 symbole manquant pour les 6 modules VR-Link qui importent le RTI (dont 126 symboles pour `vlHLA1516e.dll`).
- Construire avec `-DHLA_MQTT_MAK_NAMES=ON` (ou prendre l'artefact CI `hla-mqtt-rti-windows-x64-mak`), puis mettre le dossier des DLL dans le `PATH` des fédérés.
- VR-Link vérifie la « version du compilateur du RTI » en lisant la version de l'éditeur de liens dans l'en-tête PE de la DLL (14.00 attendu). Avec une DLL liée par VS2019 (14.29) : f18 s'arrête (contournable par `--noRtiCompilerCheck`), netdump plante (exception non attrapée). `HLA_MQTT_MAK_NAMES` met ce champ à 14.00 après l'édition de liens (vérifié : netdump découvre le F-18 et reçoit ses 45 attributs avec la DLL du build).
- Résultat : création de la fédération avec les modules RPR, join des deux fédérés, découverte de l'F-18 par netdump et réception de ses mises à jour (45 attributs RPR décodés).
- VR-Link n'envoie de mises à jour qu'après `turnUpdatesOnForObjectInstance` : fonctionne depuis les callbacks d'avis (avant : il fallait `--ignoreAdvisories`).
- Avertissements de l'essai sur les dimensions et classes MOM (`HLAfederate`, `HLAserviceGroup`, `HLAmanager.*`) absentes : le MIM standard est désormais chargé (non revérifié avec VR-Link).

## Paramètres de connexion

Ils passent par l'argument standard `localSettingsDesignator` de `connect()`, sans modification du fédéré :

```
""                                              -> variable HLA_MQTT_SETTINGS, sinon valeurs par défaut
"mqtt://broker:1883"
"mqtt://broker:1883;clientId=fed1;prefix=hla;connectTimeout=5;keepAlive=20"
"mqtt://broker:1883;reconnect=60;staleClientDelay=300"
"broker=mqtts://broker:8883;username=u;password=p"   (nécessite with_ssl)
```

Le broker par défaut est donné par la variable d'environnement `HLA_MQTT_BROKER`, sinon `tcp://localhost:1883`.
Une clé inconnue provoque `InvalidLocalSettingsDesignator`, un broker injoignable `ConnectionFailed`.

| Clé | Défaut | Rôle |
|---|---|---|
| `reconnect` | 30 | durée (s) pendant laquelle la connexion est retentée après une perte inattendue, avant `connectionLost` ; 0 : `connectionLost` immédiat |
| `staleClientDelay` | `reconnect` + 2 × `keepAlive` | durée (s) au-delà de laquelle une présence `offline` est effacée du broker |

### Reconnexion

Après une perte inattendue de la connexion, le transport retente toutes les secondes pendant `reconnect` secondes. Les publications faites entre-temps sont mises en tampon par Paho et envoyées à la reconnexion. À la reconnexion :

- les abonnements sont refaits (session propre), suivis d'un aller-retour vers le broker : tous les enregistrements retained sont alors revenus, et ceux qui manquent (fédéré parti, objet supprimé, présence effacée pendant la coupure) sont traités comme supprimés ;
- la présence repasse à `online` ; les enregistrements d'information des objets possédés (et l'objet MOM `HLAfederate`) sont republiés : les autres, qui avaient retiré ces objets à la réception du Last Will, les redécouvrent ;
- si l'exécution a disparu ou si l'enregistrement du fédéré a été repris par un autre client, le fédéré reçoit `connectionLost` (il n'est plus membre).

Le fédéré n'est pas averti d'une coupure surmontée. Pendant la coupure, ses appels de services synchrones échouent (`RTIinternalError`), et les messages non retained publiés par les autres (mises à jour, interactions) sont perdus. Les autres le considèrent comme absent : les points de synchronisation et la gestion du temps ne l'attendent pas.

### Présences périmées

Chaque client abonné aux présences note l'instant où une présence passe à `offline` (Last Will) et l'efface (publication retained vide) si elle l'est toujours après `staleClientDelay`. Les présences `offline` laissées par d'anciens processus sont donc effacées peu après la connexion d'un fédéré. Absente et `offline` ont le même sens. Un client qui voit sa propre présence effacée ou `offline` alors qu'il est connecté la republie `online`.

## Topics MQTT

Tous les topics sont retained, sauf `sync`. `F` est le nom de la fédération, encodé en pourcentage (`/`, `+`, `#`, espaces et non-ASCII).

| Topic | Contenu |
|---|---|
| `<prefix>/clients/<clientId>` | `{"state":"online",...}` à la connexion ; Last Will `{"state":"offline",...}` publié par le broker en cas de perte ; payload vide à la déconnexion propre, ou après `staleClientDelay` à l'état `offline` |
| `<prefix>/sync/<clientId>` (non retained) | aller-retour privé vers le broker (barrière) |
| `<prefix>/federations/F/execution` | `{"name","timeImplementation","modules":[hash...],"createdBy",...}` |
| `<prefix>/federations/F/fom/<hash>` | contenu XML d'un module FOM (hash = FNV-1a 64 bits des octets) |
| `<prefix>/federations/F/federates/<handle>` | `{"name","type","handle","clientId","state":"joining"/"joined"}` |
| `<prefix>/federations/F/claims/<clientId>` | réservation transitoire du nom pendant une création |
| `<prefix>/federations/F/names/<nom>/<clientId>` | réservation d'un nom d'instance `{"clientId","federate","state":"reserving"/"reserved"}` |
| `<prefix>/federations/F/objects/<chemin de classe>/@<instance>/info` | enregistrement de l'instance `{"instance","class","name","owner","clientId","owned","regions"}` |
| `.../@<instance>/update`, `.../@<instance>/delete` (non retained) | mise à jour d'attributs, suppression (binaire, `Wire.h`) |
| `<prefix>/federations/F/interactions/<chemin de classe>/@` (non retained) | interaction (binaire) |
| `<prefix>/federations/F/requests/<fédéré>`, `.../classrequests/<chemin>/@` (non retained) | requêtes de mise à jour d'attributs |
| `<prefix>/federations/F/sync/<label>/point`, `.../achieved/<fédéré>` | point de synchronisation, réalisations |
| `<prefix>/federations/F/decl/<fédéré>` | abonnements du fédéré (pour les avis) |
| `<prefix>/federations/F/time/<fédéré>` | état temporel : temps, lookahead, avance demandée, promesse, versions vues |

Le chemin de classe a un niveau de topic par composant du nom qualifié (`HLAobjectRoot/Vehicle/Car`).
S'abonner à une classe, c'est s'abonner à `<chemin>/#` : le broker transmet aussi les sous-classes et filtre le reste.
Seule une couverture minimale est abonnée (jamais une classe avec un de ses ancêtres).

### Fédération sans RTI central

- **Handles de fédérés** : FNV-1a 64 de `F|<clientId>` ; uniques tant que les clientId le sont.
- **Handles de classes, attributs, interactions, paramètres, dimensions** : FNV-1a 64 du nom qualifié (`O|HLAobjectRoot.A.B`, `A|<classe déclarante>|<attribut>`...). Chaque fédéré les calcule seul, à l'identique ; une collision dans un FDD donne `InconsistentFDD`.
- **FDD partagé** : les modules sont publiés tels quels ; un fédéré qui joint n'a pas besoin des fichiers du créateur. Les modules ajoutés au join sont fusionnés en direct par les fédérés déjà présents.
- **Opérations exclusives** (créer une fédération, prendre un nom de fédéré) : publication de la réservation, barrière (aller-retour vers le broker), examen des concurrents. Qui voit un concurrent se retire et réessaie après un délai aléatoire. Deux concurrents peuvent se retirer ensemble, jamais gagner ensemble.
- **destroy** retire l'enregistrement d'exécution puis revérifie les fédérés : un join concurrent apparaît alors (destroy est annulé, `FederatesCurrentlyJoined`) ou constate la disparition (`FederationExecutionDoesNotExist`).
- **Fédéré disparu** sans resign (crash, perte réseau) : sa présence passe à `offline` ou disparaît, son enregistrement est ignoré (équivalent d'un resign automatique) et effacé par destroy.
- Formats FOM lus : IEEE 1516-2010 et 1516-2000. Un FOM est un fichier ou une URL `file:`.

### Objets et interactions

- **Découverte** par l'enregistrement retained `info` : un abonné tardif découvre aussi les objets déjà enregistrés. La classe connue est la superclasse abonnée la plus proche ; seuls les attributs abonnés pour cette classe sont reflétés.
- **Interactions** reçues comme la classe abonnée la plus proche, avec les seuls paramètres de cette classe. Un fédéré ne reçoit pas ses propres messages.
- **Propriété** : le fédéré qui enregistre possède les attributs publiés à l'enregistrement et HLAprivilegeToDeleteObject. Pas encore de transfert de propriété.
- **Resign** : `DELETE_OBJECTS` et variantes suppriment les objets ; `UNCONDITIONALLY_DIVEST_ATTRIBUTES` les laisse sans propriétaire ; `NO_ACTION` lève `FederateOwnsAttributes` s'il en reste. Un fédéré disparu (Last Will) voit ses objets retirés chez les autres.
- **Variantes horodatées** acceptées et livrées en ordre de réception avec leur temps, puisqu'aucun fédéré ne peut encore être régulateur.
- Les mises à jour et interactions partent sans attendre l'accusé du broker (QoS 1, ou QoS 0 si tous les attributs ou l'interaction sont `HLAbestEffort`) ; Paho conserve l'ordre.
- **Avis** : chaque fédéré publie ses abonnements (`F/decl/<fédéré>`, retained). Chacun en déduit pour lui-même `start/stopRegistrationForObjectClass` (abonnement à la classe ou à une superclasse), `turnInteractionsOn/Off`, `turnUpdatesOn/OffForObjectInstance` (attributs possédés reçus par un abonné, à la classe sous laquelle il connaît l'objet) et `attributesInScope` à la découverte. Valeurs initiales des commutateurs : `<switches>` du FOM, désactivés sinon.
- **DDM (régions)** : les régions sont locales au fédéré qui les crée. Handle de région = 32 bits hauts du handle de fédéré + compteur, ce qui permet de reconnaître la région d'un autre fédéré (`RegionNotCreatedByThisFederate`) sans échange. Une région neuve couvre `[0, borne)` sur chaque dimension ; `setRangeBounds` modifie les étendues en attente, `commitRegionModifications` les rend effectives. Les régions de mise à jour d'un objet voyagent dans son enregistrement `info`, en étendues validées (`"regions": {"<attribut>": [{"<dimension>": [inf, sup]}, ...]}`), republié à chaque association, dissociation ou validation. Un abonnement avec régions abonne l'attribut ou l'interaction ; il coexiste avec l'abonnement sans région (région par défaut, qui recouvre tout).
- **Filtrage DDM (côté récepteur)** : deux régions se recouvrent si leurs intervalles se chevauchent sur chaque dimension commune ; pas de région de mise à jour = région par défaut. Une valeur d'attribut est réfléchie si l'abonné est abonné sans région, ou si une de ses régions recouvre une région de mise à jour de l'attribut. Une interaction envoyée avec `sendInteractionWithRegions` porte les étendues de ses régions dans le message. `requestAttributeValueUpdateWithRegions` ne fait fournir que les attributs dont les régions de mise à jour recouvrent celles de la requête. Les abonnements avec régions seules sont publiés dans `F/decl/<fédéré>` (`"or"`) : le propriétaire en tient compte pour `turnUpdatesOn/Off`. L'abonné reçoit `attributesInScope` / `attributesOutOfScope` quand régions, abonnements ou propriété changent. La découverte n'est pas filtrée : s'abonner à la classe suffit. `conveyRegionDesignatorSets` (régions dans les informations supplémentaires) n'est pas géré.
- **Ordre et transport choisis par l'émetteur** : `changeAttributeOrderType` / `changeInteractionOrderType` et `request*TransportationTypeChange` ne valent que pour les messages du fédéré qui les demande (confirmation par callback, immédiate). Les autres l'apprennent par les enregistrements retained : `"transport"` dans l'enregistrement `info` de l'objet, `"it"` dans `F/decl/<fédéré>` pour les interactions ; les réflexions et interactions reçues portent ce transport, et `query*TransportationType` le rapporte. `getUpdateRateValueForAttribute` rend le taux du désignateur utilisé à l'abonnement (0 sans désignateur).

### Points de synchronisation

Le label est réservé comme un nom de fédération (réservation + barrière). Chaque membre décide seul que le point est synchronisé : tous les membres l'ont atteint ou ont quitté la fédération. Le déclarant efface ensuite les enregistrements, ce qui libère le label. L'ensemble est figé à la déclaration (fédérés joints, ou l'ensemble donné) : un fédéré qui joint après n'en fait pas partie.

### Gestion du temps

- Chaque fédéré régulateur publie une **promesse** : borne inférieure des estampilles de ses futurs messages TSO (temps + lookahead, ou temps demandé + lookahead pendant une avance ; ouverte si lookahead nul après TAR/NER). Le GALT d'un fédéré est le minimum des promesses des autres régulateurs.
- C'est sûr parce que le broker conserve l'ordre des messages d'un émetteur vers un récepteur : un message TSO envoyé avant une promesse est reçu avant elle. Toutes les publications passent par une file unique (outbox) servie par un thread dédié.
- **Lookahead nul** : les promesses seules se bloquent (deadlock des messages nuls). Quand la coupe formée par les états est cohérente (chaque régulateur attend dans une avance et ses versions vues sont les dernières connues), personne ne peut être accordé avant le minimum des prochains temps : les promesses sont relevées à cette borne.
- Activer la régulation ou la contrainte attend un aller-retour vers le broker après la publication de l'état en attente ; le temps d'un nouveau régulateur est relevé au plus grand temps des fédérés contraints.
- Messages TSO livrés avant le grant, dans l'ordre des estampilles ; messages RO retenus pour un fédéré contraint hors avance, sauf livraison asynchrone.
- Pendant une avance, un régulateur ne peut envoyer en TSO qu'au-delà de son prochain temps + lookahead (plus strict que la norme, nécessaire à la règle de coupe).
- **Retract** : l'émetteur garde ses messages TSO dont l'horodatage dépasse encore son temps logique ; `retract` publie un message `Retract` sur le topic de l'original (même émetteur, même topic : le broker le livre après). Le récepteur retire le message de sa file TSO, ou appelle `requestRetraction` s'il l'a déjà livré TSO. Seuls les messages envoyés TSO sont rétractables (ni la partie RO d'une mise à jour, ni les suppressions).

### MOM (Management Object Model)

- **MIM standard** : `mim/HLAstandardMIM.xml` (IEEE 1516.1-2010) est intégré à la DLL et fusionné en premier à la création d'une fédération sans MIM ; `createFederationExecutionWithMIM` le remplace. La dimension `HLAfederate` (sans borne dans le MIM) va jusqu'à 2^32-1, borne respectée par `normalizeFederateHandle`.
- **HLAmanager.HLAfederate** : chaque fédéré joint enregistre son instance (propriétaire : le fédéré, valeurs calculées par le RTI). Elle est mise à jour sur `requestAttributeValueUpdate` et à chaque période `HLAsetTiming`, et supprimée au resign. Le fédéré ne peut pas la mettre à jour lui-même. Les compteurs (mises à jour, réflexions, interactions, instances) et l'état du temps viennent de l'ambassador.
- **HLAmanager.HLAfederation** : construite localement par chaque fédéré à partir des enregistrements retenus (exécution, modules FOM, fédérés) ; découverte dès que la classe est souscrite, réfléchie sur requête. `HLAcurrentFDD` contient les modules fusionnés (MIM en tête).
- **Interactions HLArequest, HLAadjust, HLAservice** : chaque fédéré reçoit ces interactions (abonnements internes) et traite celles dont `HLAfederate` est son handle ; le fédéré n'a pas à publier les rapports. Les services `HLAservice` s'exécutent sur le thread du fédéré, au prochain `evokeCallback` (ou par le dispatcher en `HLA_IMMEDIATE`) ; un échec produit `HLAreportMOMexception`. Les requêtes de niveau fédération (points de synchronisation, modules FOM, MIM) sont traitées par le fédéré vivant de plus petit handle, qui envoie aussi `HLAreportFederateLost` quand un fédéré disparaît sans resign.
- `HLAsetSwitches` : `conveyProducingFederate` renseigne le fédéré producteur dans les informations supplémentaires de `reflectAttributeValues` et `receiveInteraction` ; `conveyRegionDesignatorSets` est seulement mémorisé. `HLAsetExceptionReporting` signale les exceptions levées par les callbacks du fédéré.
- Limites : `HLAreportServiceInvocation` ne couvre que les services invoqués par `HLAservice` (pas les appels directs du fédéré) ; `HLAmodifyAttributeState` et les services de propriété et de sauvegarde/restauration répondent `HLAreportMOMexception` (non implémentés) ; les attributs de sauvegarde de `HLAfederation` sont vides.

## Client de test

`hla-test-federate` n'utilise que `RTI/RTI1516.h`. Le même exécutable peut donc tourner contre une autre DLL 1516e.

```bat
:: Broker local, par exemple : mosquitto -v
hla-test-federate --selftest
hla-test-federate --selftest --settings "mqtt://192.168.1.10:1883"

:: Reste connecté 60 s ; arrêter le broker pour voir le callback connectionLost
hla-test-federate --settings "mqtt://localhost:1883;keepAlive=2" --model evoked --wait 60
```

`ctest` lance `--selftest`, `hla-encoding-test` (sans broker) et `hla-federation-test`, qui suppose un broker joignable (`--broker mqtt://hôte:1883` ou `HLA_MQTT_BROKER`). Ce dernier vérifie les erreurs de création et de join, les handles identiques entre fédérés, les modules additionnels, destroy avec un fédéré disparu, et des courses (4 créations ou 4 joins simultanés sous le même nom : un seul gagnant).
`hla-time-test` (broker requis) couvre les points de synchronisation (ensemble complet ou explicite, label non unique, membre absent, membre disparu, réutilisation du label) et la gestion du temps : activation, livraison TSO ordonnée, borne d'envoi, messages RO retenus, livraison asynchrone, régulateur tardif, boucle TAR à lookahead 1 et boucle NER à lookahead nul entre deux fédérés, NER sans message résolu par la coupe cohérente, `retract` (avant et après livraison, trop tard) et changement d'ordre des attributs et interactions.
`hla-ddm-test` (broker requis) couvre création, bornes, validation et suppression de régions, régions d'un autre fédéré, enregistrement avec régions, association et dissociation, abonnements d'objets et d'interactions avec régions et leur coexistence avec l'abonnement sans région, puis le filtrage par recouvrement (déplacement des régions de mise à jour et d'abonnement), les avis de portée et de pertinence, `sendInteractionWithRegions` et `requestAttributeValueUpdateWithRegions`.
`hla-mom-test` (broker requis) couvre le MIM standard (classes, dimensions, remplacement par un MIM explicite), la découverte et les valeurs des objets `HLAfederate` et `HLAfederation`, les requêtes et rapports (publications, mises à jour envoyées, informations d'instance, modules FOM, points de synchronisation, MIM), `HLAsetTiming`, `HLAsetSwitches`, les rapports d'invocation et d'exception, les services `HLAservice` (succès, échec, paramètre indécodable) et `HLAreportFederateLost`.
`hla-robustness-test` (broker requis) coupe la connexion d'un fédéré en reprenant son clientId avec un client MQTT brut : reconnexion sans `connectionLost`, redécouverte de ses objets par les autres, mises à jour dans les deux sens, objet supprimé pendant la coupure ; `reconnect=0` ; présence `offline` d'un processus tué effacée après `staleClientDelay`.
`hla-objects-test` (broker requis) couvre découverte, réflexion filtrée, abonné tardif, interactions, variantes horodatées, réservation de noms, requêtes de mise à jour, suppression locale et redécouverte, fédéré disparu, actions de resign, changement et requête de transport (attributs et interactions), taux de mise à jour, et 1000 mises à jour reçues dans l'ordre.

Résultat attendu :

```
[PASS] createRTIambassador
[PASS] connect with invalid designator -- InvalidLocalSettingsDesignator: unknown setting: thisKey
[PASS] connect to unreachable broker -- ConnectionFailed: ...
[PASS] connect (HLA_EVOKED)
[PASS] second connect -- AlreadyConnected: ...
...
ALL TESTS PASSED (0 failure(s))
```

## Intégration continue

`.github/workflows/ci.yml` (GitHub Actions), à chaque push sur `main` et à chaque pull request :

| Job | Runner | Contenu |
|---|---|---|
| Windows MSVC Release, Debug | `windows-2022` (VS2022, v143) | Conan 2, build, Mosquitto (installateur officiel), `ctest`, exports de la DLL, artefacts `rti1516e.dll`/`fedtime1516e.dll` (Release) |
| Linux GCC Release | `ubuntu-24.04` (GCC 13) | Conan 2, build, Mosquitto (conteneur `eclipse-mosquitto:2`), `ctest` |

Le broker de la CI utilise `broker/mosquitto-hla.conf` et les tests lisent `HLA_MQTT_BROKER`.
La CI compile avec v143, binairement compatible avec les fédérés v142 ; la cible locale reste VS2019.
Le même enchaînement en local avec Conan 2 :

```bat
conan install . -s build_type=Release -s compiler.cppstd=17 --build=missing
cmake --preset conan-default
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure
```

## Ajouter un service

1. Implémenter la méthode dans `MqttRTIambassador.cpp` (ou le fichier du groupe de services).
2. Ajouter son nom à `IMPLEMENTED` dans `tools/gen_rti_sources.py`.
3. Lancer `python tools/gen_rti_sources.py` (régénère `generated/`).

## Suite prévue

- Gestion de la propriété, sauvegarde/restauration (et leurs parties MOM).

---
Headers : *Reprinted with permission from IEEE 1516.1(TM)-2010.*
