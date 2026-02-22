# Plan de migration de la sortie (document vivant)

> Regle importante: ce plan doit etre mis a jour au fur et a mesure de son execution (statuts, decisions, resultats de tests, ecarts).

## Resume

Objectif: faire evoluer la sortie du programme pour afficher, pour la solution optimale, la liste des ordres retenus triee par date de depart, avec cumul progressif:

```text
<ID> <cumul>
```

Exemple attendu:

```text
0 10
3 18
```

## Changement d'interface publique

1. Entree: inchangee (`id start duration value`).
2. Sortie valide: passe d'une valeur unique (`18`) a une sortie multi-ligne.
3. Format strict de chaque ligne valide: `"<id> <cumul>\n"` (pas d'espace final, newline finale obligatoire).
4. Sortie invalide: conservee a `Invalid input`.
5. CLI: inchangee.

## Possibilites d'implementation (apres validation des tests)

> Choix retenu pour la suite: **Option C** (continuer a exploiter la logique 6502).
>
> Note de recommandation personnelle (avant arbitrage): je preconisais initialement **Option A** pour la maintenabilite, mais le plan est maintenant aligne sur le choix **Option C**.

### Option A (recommandation personnelle initiale)

Implementer la selection optimale et la reconstruction de trajectoire cote C++ hote.

- Avantages: lisibilite, maintenabilite, evolution plus simple.
- Inconvenient: logique metier moins deleguee au blob 6502.

### Option B

Conserver le calcul actuel et ajouter un calcul parallele cote C++ pour reconstruire la liste optimale.

- Avantage: transition progressive.
- Inconvenient: double logique a maintenir.

### Option C

Modifier la logique 6502 embarquee pour produire directement la nouvelle sortie.

- Avantage: continuite historique.
- Inconvenient: cout et risque plus eleves.
- Contrainte forte: pas de source assembleur originale disponible dans le repo; les modifications doivent donc s'appuyer sur le blob present dans `src/ram.h`.
- Statut: **option retenue pour implementation**.

## Regles de mise a jour continue

1. A chaque action terminee, mettre a jour la colonne `Statut` (`A_FAIRE`, `EN_COURS`, `FAIT`, `BLOQUE`).
2. Reporter tout changement de choix fonctionnel dans `Decisions`.
3. Ajouter une entree dans `Journal d'execution` a chaque execution de tests.
4. En cas d'ecart, creer une ligne `BLOQUE` avec action corrective.
5. Conserver une tracabilite simple: date, action, resultat, decision.

## Plan d'execution (tests d'abord)

| ID | Etape | Objectif | Statut | Livrable | Critere de validation |
|---|---|---|---|---|---|
| E1 | Migrer les tests valides existants | Mettre a jour tous les tests valides vers `id cumul` (parser, ids, calendar, bounds, scheduling) | A_FAIRE | Specs TOML mises a jour | Les sorties attendues valides sont au nouveau format |
| E2 | Ajouter des tests scheduling cibles | Couvrir l'exemple fourni + tri chrono + cumul multi-ligne + cas `year>=128` + tie-break deterministe | A_FAIRE | Nouveaux cas `SCH-*` | Les cas couvrent les besoins metier de la nouvelle sortie |
| E3 | Nettoyer les attentes obsoletes | Supprimer/remplacer les attentes valides au format scalaire | A_FAIRE | Specs coherentes | Aucune attente valide au format ancien |
| E4 | Verifier la direction | Executer la suite modifiee avant implementation | A_FAIRE | Rapport d'execution | Les invalides restent OK, les valides migres echouent tant que non implemente |
| E5 | GO implementation (Option C) | Demarrer le dev en modifiant la logique 6502 (blob `src/ram.h`) apres validation des tests | A_FAIRE | Decision explicite | GO utilisateur obtenu |

## Modifications de tests prevues (E1 + E2)

### Fichiers concernes

- `/Users/lou-theo/Experimentations/bare/tests/spec/scheduling.toml`
- `/Users/lou-theo/Experimentations/bare/tests/spec/parser.toml`
- `/Users/lou-theo/Experimentations/bare/tests/spec/ids.toml`
- `/Users/lou-theo/Experimentations/bare/tests/spec/calendar.toml`
- `/Users/lou-theo/Experimentations/bare/tests/spec/bounds.toml`

### Regle de conversion des cas valides

- Ancien attendu: `"N \n"`
- Nouveau attendu: une ou plusieurs lignes `"<id> <cumul>\n"` triees par date de depart.
- Format strict: aucun espace final sur les lignes, newline finale obligatoire.

### Cas additionnels a ajouter (scheduling)

1. Cas fourni:

```text
0 0 5 10
1 3 7 14
2 5 9 7
3 6 9 8
```

Expected:

```text
0 10
3 18
```

2. Entree non triee + verification tri de sortie.
3. Plusieurs ordres retenus + verification du cumul progressif.
4. Cas avec annees eloignees (`>=128`) pour couvrir la reecriture calendrier.
5. Cas d'egalite de valeur optimale pour valider le tie-break deterministe.

## Strategie de verification (E4)

1. `python3 /Users/lou-theo/Experimentations/bare/tests/run.py --list`
2. `make test`
3. Resultat attendu avant implementation:
- Cas invalides: passent.
- Cas valides migres: echouent (normal, direction confirmee).

## Scenarios a couvrir

1. Optimal unique.
2. Optimal multi-ordres.
3. Entree desordonnee, sortie ordonnee.
4. Cumul progressif correct a chaque ligne.
5. Validation d'entree invalide inchangee (`Invalid input`).
6. Non-regression calendrier/bornes/IDs.

## Decisions

- Perimetre: toutes categories de tests.
- Sortie invalide conservee: `Invalid input`.
- Regle de departage en cas d'egalite de valeur optimale: choisir la solution dont le **premier ID de sortie est le plus bas**.
- Si plusieurs solutions ont le meme premier ID, comparer ensuite le 2e ID, puis le 3e, etc. (ordre lexicographique sur la sequence d'IDs de sortie) pour garantir un resultat deterministe.
- Si une sequence est prefixe exact d'une autre, la sequence la plus courte est retenue.
- Option d'implementation retenue: **Option C** (logique 6502 conservee).
- Recommandation personnelle initiale du redacteur: **Option A** (non retenue).
- Contrainte d'implementation confirmee: pas de source assembleur 6502 originale; s'appuyer au maximum sur `src/ram.h` et valider par tests de non-regression.
- Format de sortie valide confirme: `"<id> <cumul>\n"` sans espace final, newline finale obligatoire.

## Journal d'execution

| Date | Etape | Action | Resultat | Decision / Suivi |
|---|---|---|---|---|
| AAAA-MM-JJ HH:MM | E0 | Creation du plan | Plan initialise | Demarrage phase tests |
| 2026-02-22 | E0 | Revue adversariale + arbitrages utilisateur | Arbitrages integres | Option C retenue, tie-break deterministe, format strict valide |

## Criteres de fin de phase "tests"

1. Toutes les specs sont alignees sur le nouveau contrat de sortie.
2. Les cas invalides restent conformes.
3. Les nouveaux cas scheduling couvrent le besoin metier.
4. Le rapport d'execution confirme une direction claire avant implementation.
