eternityII
==========
Serveur :
tcpserver 80 ./pieces.csv

Client :
tcpclient localhost ./pieces.csv


TODO :
- gestion du ctrl+c pour quitter en envoyer les infos au serveur ou en sauvegardant un backup spécifique pour le serveur
- multithreading a débugger pour comprendre les problèmes de perfs
- gestion des instructions sous forme de tableau de fonctions (["command", "function"])
- client chargé d'effectuer du rmnonext pour diminuer le nombre de possibilité sur le serveur sans que lui s'en charge
- voir pour une présentation des stats en permanence et qui ne fait pas défiler la console