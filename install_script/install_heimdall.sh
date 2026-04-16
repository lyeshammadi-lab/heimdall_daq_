#!/bin/bash

# ==============================================================================
# Script for installing Heimdall DAQ (CUstomized for local data saving)
# Target : Raspberry Pi 4 (ARM64 / aarch64) - OS 64-bit required
# ==============================================================================

# Arrêter le script immédiatement si une commande échoue
set -e

echo "========================================"
echo " 1. Mise à jour et dépendances de base"
echo "========================================"
sudo apt-update
sudo apt-get install -y build-essential git cmake libusb-1.0-0-dev lsof libzmq3-dev unzip wget

cd $HOME
wget https://github.com/joan2937/pigpio/archive/master.zip
unzip -q master.zip
rm master.zip
cd pigpio-master
make -j4
sudo make install

echo "========================================"
echo " 2. Installation du driver RTL-SDR custom"
echo "========================================"
cd $HOME
if [ ! -d "librtlsdr" ]; then
    git clone https://github.com/krakenrf/librtlsdr
fi
cd librtlsdr
sudo cp rtl-sdr.rules /etc/udev/rules.d/rtl-sdr.rules
mkdir -p build && cd build
cmake ../ -DINSTALL_UDEV_RULES=ON
make -j4
sudo ln -sf $HOME/librtlsdr/build/src/rtl_test /usr/local/bin/kraken_test
# Blacklist du driver par défaut
echo 'blacklist dvb_usb_rtl28xxu' | sudo tee /etc/modprobe.d/blacklist-dvb_usb_rtl28xxu.conf > /dev/null

echo "========================================"
echo " 3. Installation de la librairie DSP Ne10 (ARM)"
echo "========================================"
cd $HOME
if [ ! -d "Ne10" ]; then
    git clone https://github.com/krakenrf/Ne10
fi
cd Ne10
mkdir -p build && cd build
cmake -DNE10_LINUX_TARGET_ARCH=aarch64 -DGNULINUX_PLATFORM=ON -DCMAKE_C_FLAGS="-mcpu=native -Ofast -funsafe-math-optimizations" ..
make -j4

echo "========================================"
echo " 4. Installation de Miniforge3 (Python)"
echo "========================================"
cd $HOME
if [ ! -d "$HOME/miniforge3" ]; then
    wget https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-Linux-aarch64.sh -O Miniforge3.sh
    chmod ug+x Miniforge3.sh
    # Installation en mode batch (-b) silencieux (-p path)
    ./Miniforge3.sh -b -p $HOME/miniforge3
fi

# Initialisation de conda pour ce script Bash
eval "$($HOME/miniforge3/bin/conda shell.bash hook)"
conda init bash
conda config --set auto_activate_base false

echo "========================================"
echo " 5. Configuration de l'environnement Conda"
echo "========================================"
# Créer l'environnement s'il n'existe pas déjà
if ! conda info --envs | grep -q 'kraken'; then
    conda create -y -n kraken python=3.9.7
fi
conda activate kraken
conda install -y scipy==1.9.3 numba==0.56.4 configparser pyzmq scikit-rf

echo "========================================"
echo " 6. Téléchargement de votre Firmware Heimdall Modifié"
echo "========================================"
cd $HOME
mkdir -p krakensdr && cd krakensdr
if [ -d "heimdall_daq_data_saver" ]; then
    echo "Le dossier heimdall_daq_data_saver existe déjà, mise à jour..."
    cd heimdall_daq_data_saver
    git pull
else
    # Clonage de VOTRE dépôt GitHub
    git clone https://github.com/lyeshammadi-lab/heimdall_daq_custom.git heimdall_daq_data_saver
    cd heimdall_daq_data_saver
fi

echo "========================================"
echo " 7. Compilation des fichiers C (Heimdall)"
echo "========================================"
cd $HOME/krakensdr/heimdall_daq_data_saver/Firmware/_daq_core/

# Copie des librairies statiques requises vers le dossier de compilation
cp $HOME/librtlsdr/build/src/librtlsdr.a .
cp $HOME/librtlsdr/include/rtl-sdr.h .
cp $HOME/librtlsdr/include/rtl-sdr_export.h .
cp $HOME/Ne10/build/modules/libNE10.a .

# Remplacer les règles pour PIGPIO automatiquement dans le Makefile (si commentées)
sed -i 's/#PIGPIO=-lpigpio -DUSEPIGPIO/PIGPIO=-lpigpio -DUSEPIGPIO/g' Makefile

# Nettoyage et compilation avec les 4 cœurs du RPi4
make clean
make -j4

echo "========================================================================="
echo " INSTALLATION TERMINÉE AVEC SUCCÈS ! "
echo "========================================================================="
echo "ATTENTION : Des règles udev et des modules noyau ont été modifiés."
echo "Un redémarrage du Raspberry Pi est OBLIGATOIRE pour qu'ils prennent effet."
echo "Tapez 'sudo reboot' pour redémarrer maintenant."
echo "========================================================================="
