# Performances

Mesures de `tools/hla_load.py bench` (issue #42) : un fédéré `hla-load` par processus, sur une seule
machine, avec un broker Mosquitto local configuré par `broker/mosquitto-hla.conf`. Build Release, 2026-11-26.

```bat
python tools\hla_load.py bench --exe build\bin\Release\hla-load.exe --markdown perf.md --json perf.json
```

Plateforme : Windows 11 (10.0.26300), AMD64 (Family 25 Model 97), 24 processeurs logiques, Mosquitto 2.x
sur la même machine (`mqtt://localhost`).

## Débit : mises à jour d'attributs

Chaque fédéré enregistre ses objets, puis met chacun à jour plusieurs fois, aussi vite que possible
(transport fiable, ordre de réception). Chaque fédéré vérifie qu'il a reçu chaque mise à jour des autres
une seule fois, dans l'ordre. Toutes les lignes ont réussi cette vérification.

- **Reflects/s** : réflexions reçues par l'ensemble des fédérés, divisées par la durée entre le début
  des envois et la dernière réception.
- **Latence** : délai d'une mise à jour entre l'envoi et la réception, dans une rafale. Les envois sont
  mis en file sans attendre ; une mise à jour attend donc derrière celles qui la précèdent. Cette valeur
  mesure la durée d'écoulement de la rafale, pas la latence d'un message isolé (voir l'aller-retour plus bas).

| Fédérés | Objets/féd. | Attributs | Charge (o) | MAJ/objet | Réflexions reçues | Réflexions/s | Latence p50 (ms) | Latence p99 (ms) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 10 | 4 | 64 | 200 | 4 000 | 4 111 | 765 | 966 |
| 4 | 10 | 4 | 64 | 200 | 24 000 | 14 902 | 1 167 | 1 599 |
| 8 | 10 | 4 | 64 | 200 | 112 000 | 36 612 | 1 976 | 3 036 |
| 16 | 10 | 4 | 64 | 200 | 480 000 | 52 089 | 5 892 | 9 165 |
| 2 | 100 | 4 | 64 | 50 | 10 000 | 4 168 | 1 887 | 2 383 |
| 2 | 1 000 | 4 | 64 | 50 | 100 000 | 2 428 | 36 036 | 41 010 |
| 2 | 10 | 16 | 64 | 50 | 1 000 | 4 265 | 177 | 231 |
| 2 | 10 | 32 | 64 | 50 | 1 000 | 4 081 | 180 | 240 |
| 2 | 10 | 1 | 1 024 | 200 | 4 000 | 4 433 | 703 | 897 |
| 2 | 10 | 1 | 16 384 | 200 | 4 000 | 4 092 | 741 | 961 |

## Latence : aller-retour d'interactions

Le fédéré 0 envoie une interaction `Ping` et attend un `Pong` de chacun des autres fédérés avant d'envoyer
la suivante : 1 000 allers-retours après 10 d'échauffement. L'horloge est celle de l'émetteur, sans
écart d'horloge entre processus.

| Fédérés | Charge (o) | Moyenne (ms) | p50 (ms) | p99 (ms) | Min (ms) | Max (ms) |
|---:|---:|---:|---:|---:|---:|---:|
| 2 | 64 | 0,575 | 0,572 | 0,701 | 0,453 | 0,782 |
| 2 | 4 096 | 0,584 | 0,586 | 0,716 | 0,489 | 0,897 |
| 4 | 64 | 0,833 | 0,832 | 0,948 | 0,691 | 1,006 |
| 8 | 64 | 1,401 | 1,415 | 1,613 | 1,184 | 1,714 |

## Lecture

- **Aller-retour** : environ 0,6 ms entre deux fédérés, soit deux traversées du broker. Avec 8 fédérés,
  l'émetteur reçoit 7 réponses par aller-retour, ce qui donne 1,4 ms.
- **Débit global** : il croît avec le nombre de fédérés (52 000 réflexions/s à 16) car chaque fédéré reçoit
  en parallèle des autres.
- **Débit par fédéré récepteur** : il plafonne entre 2 000 et 3 300 messages/s. La taille de la charge
  (64 o à 16 Kio) et le nombre d'attributs (1 à 32) le changent peu ; le coût est donc par message.
  Il baisse avec le nombre d'objets connus : 4 168/s avec 100 objets, 2 428/s avec 1 000.
- **Fenêtre QoS 1 du broker** : l'élargir (`max_inflight_messages 1000` au lieu de 20) ne change pas
  le débit, ce n'est donc pas elle qui limite.
- **Pistes d'optimisation** : profiler le traitement d'un message reçu, en particulier les parcours
  qui dépendent du nombre d'objets, et la file d'envoi (un `publish` Paho par message).

## Broker

Le broker borne la file de chaque client. Par défaut, Mosquitto garde 1 000 messages
(`max_queued_messages`) et jette les messages QoS 1 au-delà. Une rafale vers un fédéré plus lent perd alors des mises à jour sans
que le RTI le sache : sur un Mosquitto configuré par défaut, 6 fédérés qui reçoivent chacun 5 000 mises à jour
en rafale en perdent ainsi, et `hla-load` signale le nombre de mises à jour reçues. `broker/mosquitto-hla.conf`
porte la limite à 100 000.

## Plusieurs machines

`tools/hla_load.py run` s'exécute sur chaque machine avec la même fédération, le même total de fédérés
et des indices disjoints (voir le README, « Charge et performances »). Ces mesures n'ont été faites
que sur une machine. Sur un réseau, l'aller-retour ajoute deux fois la latence réseau vers le broker.
