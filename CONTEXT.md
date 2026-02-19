# Contexte du projet `bare`

## Démarrage rapide

Compiler:

```sh
make bbr
```

Lancer avec une entrée fournie en pipeline:

```sh
echo "1 0 3 10 2 1 1 8 3 3 1 5" | ./bin/bbr
```

Lancer en interactif:

```sh
./bin/bbr
# saisir les nombres puis Ctrl-D pour terminer l'entrée
```

## Ce qu'est réellement ce programme

- `lib/mos6502.cpp` et `lib/mos6502.h`: émulateur CPU MOS 6502.
- `src/bare.cpp`: couche hôte C++ (mémoire, I/O, boucle d'exécution).
- `src/ram.h`: binaire 6502 embarqué (`binary[]`) contenant la logique métier.

En clair: le C++ sert surtout de "machine virtuelle", et le comportement métier est dans le blob 6502.

Exception importante: pour les entrées contenant des années >= 128, `src/bare.cpp` applique un pré-traitement des intervalles avant exécution du blob, afin d'éliminer la cyclicité temporelle 128 ans de la représentation interne 16 bits.

## Modèle d'entrée/sortie

Dans `src/bare.cpp`:

- lecture d'un caractère d'entrée sur le port mémoire `0x00FE` (lié à `stdin`)
- écriture d'un caractère de sortie sur `0x00FF` (lié à `stdout`)
- arrêt du programme via écriture sur `0x00F9`

Le binaire n'attend pas d'arguments CLI.

## Format d'entrée inféré

Le programme lit des entiers décimaux non signés, avec un parseur **ligne par ligne**.

Chaque ligne doit contenir un nombre de tokens multiple de 4, au format:

`id start duration value`

Contraintes observées:

1. Chaque ligne non vide doit avoir un nombre de tokens multiple de 4.
2. Tous les tokens doivent être numériques (chiffres décimaux).
3. Une ligne trop longue est rejetée (limite observée: 256 caractères max par ligne).
4. `id` doit être unique (contrôle effectif sur `id & 0xFFFF`, donc collision modulo 65536).
5. `start` doit respecter un encodage calendrier interne valide.
6. `duration` doit être dans `1..10000`.
7. `value` doit être dans `1..100000`.

Si une contrainte échoue, la sortie est:

```text
X
```

## Problème résolu: "weighted interval scheduling"

Le programme résout un problème d'optimisation de planning:

1. Chaque bloc `(id, start, duration, value)` décrit un intervalle temporel.
2. Cet intervalle "occupe" une période de `duration` unités à partir de `start`.
3. Deux intervalles qui se chevauchent ne peuvent pas être choisis ensemble.
4. L'objectif est de choisir un sous-ensemble d'intervalles sans chevauchement qui maximise la somme des `value`.
5. La sortie est cette somme maximale.

Intuition: ce n'est pas "prendre la plus grande valeur individuelle", mais "trouver la meilleure combinaison compatible".

## Explications des exemples d'input/output

Exemple 1:

```sh
printf "1 2 3 4\n" | ./bin/bbr
# -> 4
```

Analyse:

1. Il n'y a qu'un seul intervalle.
2. Il est valide.
3. Le meilleur total possible est donc sa valeur: `4`.

Exemple 2:

```sh
printf "1 2 3 4 5 6 7 8\n" | ./bin/bbr
# -> 12
```

Analyse:

1. Deux intervalles sont fournis.
2. Dans ce cas, ils ne se chevauchent pas.
3. Le programme peut garder les deux: `4 + 8 = 12`.

Exemple 3:

```sh
printf "1 0 5 100 2 0 2 60 3 2 1 60\n" | ./bin/bbr
# -> 120
```

Analyse:

1. Intervalle A: valeur `100`, plus long.
2. Intervalles B et C: valeurs `60` et `60`, compatibles entre eux.
3. A chevauche B/C, donc choix exclusif entre "A" ou "B+C".
4. `B + C = 120` est meilleur que `100`.
5. Sortie: `120`.

Exemple invalide:

```sh
printf "1\n" | ./bin/bbr
# -> X
```

Analyse:

1. L'entrée ne contient qu'un token.
2. Le programme attend des blocs de 4 tokens.
3. Validation échoue, sortie `X`.

## Détail: gestion des années bissextiles

Le champ `start` n'est pas une date texte, c'est un entier encodé:

1. `year = start >> 9`
2. `day = start & 0x1FF` (donc `0..511`)

Interpretation importante: `day` est un index de jour sur base 0.

1. `day=0` = premier jour de l'annee.
2. Annee non bissextile: derniers index valides `0..364` (365 jours).
3. Annee bissextile: derniers index valides `0..365` (366 jours).
4. `day=366` serait un 367e jour: toujours invalide.

Règle observée pour valider `start`:

1. `year` doit être dans `0..100000`.
2. Une année est bissextile si `year % 4 == 0`, sauf les siècles (`year % 100 == 0`) qui ne sont pas bissextiles, sauf ceux divisibles par `400`.
3. Si l'année est bissextile, alors `day <= 365`.
4. Sinon, `day <= 364`.

Exemple détaillé A (valide, année bissextile):

```sh
printf "1 4461 1 99\n" | ./bin/bbr
```

1. `4461 >> 9 = 8`, donc `year=8`.
2. `4461 & 511 = 365`, donc `day=365`.
3. `8 % 4 == 0`, donc `day=365` est autorisé.
4. La ligne est valide, sortie `99`.

Exemple détaillé B (invalide, année non bissextile):

```sh
printf "1 4973 1 99\n" | ./bin/bbr
```

1. `4973 >> 9 = 9`, donc `year=9`.
2. `4973 & 511 = 365`, donc `day=365`.
3. `9 % 4 != 0`, donc max autorisé = `364`.
4. La validation échoue, sortie `X`.

Exemple détaillé C (frontière fin d'année -> début suivante, sans chevauchement):

```sh
printf "1 2413 1 10 2 2560 1 20\n" | ./bin/bbr
```

1. `2413 = year4 day365` (dernier jour d'une année bissextile).
2. `2560 = year5 day0` (premier jour de l'année suivante).
3. Durée `1` pour les deux intervalles, donc ils sont adjacents mais non chevauchants.
4. Le programme peut prendre les deux: sortie `30`.

Exemple détaillé D (même frontière, avec chevauchement):

```sh
printf "1 2413 2 10 2 2560 1 20\n" | ./bin/bbr
```

1. Le premier intervalle (durée `2`) couvre fin année 4 et début année 5.
2. Le second démarre exactement au début de l'année 5.
3. Ils se chevauchent, donc incompatibles.
4. Le programme garde la meilleure valeur seule: sortie `20`.

Résultat de campagne de tests bissextiles et bornes:

1. Les règles bissextiles grégoriennes sont appliquées (ex: 1900/2100 non bissextiles, 2000/2400 bissextiles).
2. `day=365` est accepté pour des années bissextiles testées, y compris `year=100000`.
3. `day=365` est rejeté pour des années non bissextiles testées (ex: `year=99999`, `year=1900`).
4. `day=366` est rejeté.
5. `year=100001` est rejeté (borne haute exclue).
6. Les cas de planification séparés de 128 ans (ou plus) sont désormais distingués correctement (plus de chevauchement artificiel cyclique).

Pourquoi on voit souvent 364 / 365 / 366 dans les tests:

1. `364` teste la borne haute d'une annee non bissextile (doit passer).
2. `365` discrimine bissextile vs non bissextile (passe seulement en bissextile).
3. `366` teste le depassement absolu (doit toujours echouer).

## Remarque

Cette spécification est reconstruite par inspection du code et tests d'exécution; il n'y a pas de spec métier explicite en commentaires. Les règles "par ligne" et la limite de longueur ont été validées expérimentalement.
