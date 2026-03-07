# TetraOS
Mon premier OS, conçu pour pouvoir l'utiliser dans la gestion de structure d'entrainement et utilisation de model d'IA entrainé dans la domotique et l'agentique (assistant virtuel-réel Tetramon en Apple Inteligence like), mais le project actuel ne contien absolument rien de tout ça pour le moment, ce n'est qu'un OS barre métal comme plein d'autre, mais pas basé sur linux ...

Mon projet est conçu pour un processeur architecture 64x86, il fonctionne en mode 32 bits et es donc probablement compatible avec d'anciennes machines (a tester).
J'interdit strictement toute forme de recopie a d'autre fin que l'experience personnel (j'entend par la que il est autoriser de recopier et utiliser TetraOS uniquement pour l'utilisation générique d'un OS, et la modification a des fins expérimental et non redistribuable en demandant quand meme avant sur mon discord trouvable dans ma bio ...).

# Compilation
TetraOS n'utilise pas encore ses propres outils de compilations (qui seront intégré a l'OS plus tard dans le proejt), pour le compilé ous aurez besoin de plusieurs outils et d'une configuration partculière en fonction de votre platforme (Windows, MacOS, ou Linux).

Voici ci dessous les détails pour la compilation a partir des différents OS disponible pour la compilation de base.

## **Windows**
Pour compiler TetraOS sous windows , vous avez besoin d'i686 dans le répertoire. Voici le lien pour le télécharger précompilé : [PreCompilated i686](https://github.com/lordmilko/i686-elf-tools/releases).

Une fois le fichier i686-elf-tools-windows.zip décompressé, renommez le répertoire en **i686**.
Les autres outils de la chaîne de compilation sous windows sont inclus dans le projet.

## **MacOS**
Pour compiler sous MacOS, vous aurez besoin d'installer via HomeBrew plusieurs packages (Qemu, Nasm et i686), voici les commandes pour l'installation de ces packages :

HomeBrew (si pas déja fait ...) :
`/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"`

Tous les outils de la chaine :
`brew install i686-elf-gcc i686-elf-binutils nasm qemu`

## **Linux**
Vous êtes assé fort pour installer les outils vous même (bon j'avoue j'ai juste la flemme d'aller chercher les commandes pour chaque distrib ou pour chaque config, deso ^_^) ...
