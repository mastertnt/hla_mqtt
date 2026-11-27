# Conformité IEEE 1516-2010 : tests d'OpenRTI

Il n'existe pas de suite de vérification 1516-2010 libre équivalente au Pitch Verifier. Ce dossier
compile donc les programmes de test d'[OpenRTI](https://github.com/onox/OpenRTI) (`tests/rti1516e`
et `tests/rti1516`) contre `rti1516e.dll` et `fedtime1516e.dll`. Ce sont des fédérés de test
ordinaires : ils n'utilisent que l'API standard, à travers la bibliothèque de test d'OpenRTI.

## Mise en place

```bat
git clone --depth 1 https://github.com/onox/OpenRTI.git D:\Projects\DA\OpenRTI
cmake -S . -B build -DHLA_MQTT_OPENRTI_SOURCE_DIR=D:/Projects/DA/OpenRTI
cmake --build build --config Release
cd build
ctest -C Release -L openrti --output-on-failure
```

Un broker joignable est requis (`HLA_MQTT_BROKER`, `mqtt://localhost:1883` par défaut), configuré
comme `broker/mosquitto-hla.conf` : les tests à 10 fédérés produisent des rafales que la file par défaut de
Mosquitto (1000 messages par client) tronque, et un message perdu fait échouer le test (barrière expirée,
accord de temps manquant). Seul le
cœur d'OpenRTI (`src/OpenRTI`) est compilé, en bibliothèque statique : il fournit aux tests leurs
utilitaires (options, threads, horloge, aléa). Le RTI d'OpenRTI n'est ni compilé ni utilisé.

Chaque test est lancé par `hla-openrti-run <préfixe> <programme> <arguments>`. Ce lanceur purge les
messages retenus sous `<préfixe>/` avant et après le test. Il fixe aussi
`HLA_MQTT_SETTINGS=prefix=<préfixe>`, ce qui isole chaque test sur le broker.

## Adaptations des sources

Les sources sont copiées dans `build/conformance/openrti/src` à la configuration, puis adaptées.
Les originaux ne sont pas modifiés.

| Adaptation | Raison |
|---|---|
| `thread://` remplacé par `""` | L'URL de connexion d'OpenRTI (RTI dans le processus) est remplacée par la configuration de ce RTI. |
| `HLA_OPENRTI_ITERATIONS` | Chaque test répète son scénario 99 fois. Cette variable réduit ce nombre (10 pour interaction-2 et 3, objectinstance-2 et concurrent-time). |
| `getFederateHandle()`, nom de la fabrique de temps | Ajoutés à la bibliothèque de test, comme l'attendent les tests 1516-2000 portés. |
| Callbacks de type de transport | Implémentations vides, sans quoi la classe de test serait abstraite. |
| Tests 1516-2000 (`tests/rti1516`) | Portés vers l'API 2010 : espace de noms `rti1516e`, `connect` avant tout service, callbacks 2010 relayés vers leurs formes 2000 (`callbacks2000.h`). |

## Écarts avec OpenRTI

Pour ces cas, la norme laisse le choix ou impose un comportement différent de celui d'OpenRTI. Le test
copié est alors adapté, et ce RTI garde le comportement de la norme.

- **Arithmétique du temps logique** (`logical-time`). Ce RTI lève `IllegalTimeArithmetic` quand
  un résultat sort de l'intervalle représentable (IEEE 1516.1-2010, 12.1). OpenRTI sature à
  la valeur finale, et le test compte sur cette saturation pour terminer ses boucles. Le test copié
  sature donc lui-même.
- **Fusion de modules FOM** (`modules`). La norme (IEEE 1516.2-2010) admet qu'un module nomme une
  classe sans la définir (« scaffolding »). Elle exige seulement que deux définitions complètes
  soient identiques. OpenRTI refuse ces modules ; le test copié ne retient que les conflits réels.
- **Publication d'un ensemble vide** (`objectname`). L'enregistrement exige que la classe soit
  publiée (6.8), donc au moins un attribut publié (5.2). OpenRTI accepte un ensemble vide. Le test
  copié publie `HLAprivilegeToDeleteObject`.

## Tests

| Test | Programme | Contenu |
|---|---|---|
| link, encode | `rti1516e/link`, `encode` | Édition de liens de toute l'API, encodage HLA |
| logical-time-1 à 3 | `rti1516e/time/logical-time` | Arithmétique et encodage des temps standard |
| fddget-* | `rti1516e/fddget` | Noms et handles des FDD (qualifiés, suffixes, versions 2000 et 2010) |
| modules-1, 2, 6, 7 | `rti1516e/modules` | Modules FOM à la création et au join, conflits, concurrence |
| create, concurrent-create | `rti1516/create` | Création et destruction, 10 créations concurrentes |
| join, concurrent-join | `rti1516/join` | Join et resign, 10 joins concurrents |
| sync-1, 2 | `rti1516/sync` | Points de synchronisation entre 1 et 10 fédérés |
| interaction-1 à 3 | `rti1516/interaction` | Abonnements changeants, classe reçue, paramètres |
| objectname-1, 2 | `rti1516/objectname` | Réservation et libération de noms d'objets |
| objectinstance-1, 2 | `rti1516/objectinstance` | Découverte par classe d'abonnement, réflexion filtrée, suppression |
| concurrent-time-L*-N* | `rti1516/concurrent-time` | 10 fédérés régulateurs et contraints, lookahead 0 ou 1, TAR, TARA, NMR, NMRA, FQR |

## Défauts trouvés par la suite

Les défauts suivants ont été corrigés dans le RTI :

- extension concurrente du FDD au join (verrou de modules FOM) ;
- destruction concurrente d'une création du même nom ;
- `HLAfloat64Time` : `T + I > T` pour tout intervalle strictement positif ;
- noms qualifiés partiels et suffixes uniques, attributs et paramètres qualifiés ;
- URL `data:` pour les modules FOM, création avec le seul MIM, racine implicite ;
- dimensions séparées par des espaces (FDD 1516-2000), règle de définition identique (1516.2-2010) ;
- callbacks déjà en file livrés après `unsubscribe` : ils sont maintenant filtrés à la livraison ;
- demande d'activation de la régulation ou de la contrainte : elle reste en attente jusqu'à la
  livraison de `timeRegulationEnabled` / `timeConstrainedEnabled` (`RequestFor...Pending`), et non
  plus jusqu'à l'accord interne ;
- nom d'objet déjà détenu demandé de nouveau : il reste réservé (auparavant, la revendication pouvait
  être retirée alors que le fédéré croyait le détenir) ;
- point de synchronisation sans ensemble explicite : annoncé aussi aux fédérés qui joignent avant la
  synchronisation ;
- activation de la régulation avec un lookahead nul : envois permis au temps courant (et non plus
  seulement au-delà), et temps d'activation placé après celui d'un fédéré contraint qui a reçu un accord
  TAR/NER à ce temps ;
- coupe cohérente (NER sans message) : un régulateur qui n'avance pas y entre avec sa promesse, au lieu
  de l'empêcher (interblocage avec un fédéré qui active sa contrainte).

## Échec connu

`concurrent-time-L0-N5` est désactivé. Avec un lookahead nul et des services d'avance tirés au hasard
(TAR, TARA, NER, NERA, FQR), il échoue à chaque exécution :
- accords trop précoces (« Time advance grant too early ») ;
- attentes sans accord (« Timeout waiting for next message »).

Chaque service seul passe, avec un lookahead nul (N0 à N4) comme avec un lookahead de 1 (L1-N5).
La combinaison de FQR et des accords « available » avec un lookahead nul reste à analyser.

## Durées

Pour se synchroniser, les tests font enregistrer un point par fédéré, que tous réalisent. Avec N
fédérés, chaque étape coûte donc N³ réceptions sur le broker (N points, N réalisations, N
destinataires). Les tests à 10 fédérés qui se synchronisent à chaque étape sont
donc longs : par exemple, sync-2 prend environ 2 min.
