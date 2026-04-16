#!/bin/bash
#
#   DAQ chain start script — KrakenSDR / HeIMDALL DAQ Firmware
#
#   Modifications vs. original :
#     - iq_server (Ethernet TCP:5000) supprimé
#     - iq_saver ajouté  → écrit les trames IQ CF32 dans _daq_raw/*.dat
#     - Vérification ports 5000/5001 supprimée (plus de serveur Ethernet)
#     - Dossier _daq_raw/ créé automatiquement
#     - FIFO _data_control/iq_saver_control créé pour contrôle start/stop
#
#   Contrôle de l'enregistrement (depuis un autre terminal) :
#     echo "START" > _data_control/iq_saver_control   # démarre
#     echo "STOP"  > _data_control/iq_saver_control   # arrête / ferme fichier
#     echo "EXIT"  > _data_control/iq_saver_control   # termine iq_saver
#
#   Lecture MATLAB (exemple, adapter num_ch et cpi_size à votre config) :
#     fid  = fopen('_daq_raw/iq_20250416_143022.dat', 'rb');
#     raw  = fread(fid, [2*5, 1048576], 'float32');   % [2*num_ch x cpi_size]
#     iq   = complex(raw(1:2:end,:), raw(2:2:end,:)); % (num_ch x cpi_size)
#     fclose(fid);
#
#   Project : HeIMDALL DAQ Firmware
#   License : GNU GPL V3
#   Authors : Tamas Peto, Carl Laufer

# ── Vérification du fichier de configuration ──────────────────────────────────
echo -e "\e[33mConfig file check bypassed [ WARNING ]\e[39m"

sudo sysctl -w kernel.sched_rt_runtime_us=-1

# ── Lecture du type d'interface de sortie ─────────────────────────────────────
out_data_iface_type=$(awk -F'=' '/out_data_iface_type/ {gsub (" ", "", $0); print $2}' daq_chain_config.ini)

# ── Création du dossier de sortie IQ brut ─────────────────────────────────────
mkdir -p _daq_raw
echo -e "\e[92mOutput directory _daq_raw/ ready [ OK ]\e[39m"

# ── (Re)création des FIFOs de contrôle ───────────────────────────────────────
# Decimateur
rm _data_control/fw_decimator_in  2> /dev/null
rm _data_control/bw_decimator_in  2> /dev/null
rm _data_control/fw_decimator_out 2> /dev/null
rm _data_control/bw_decimator_out 2> /dev/null
mkfifo _data_control/fw_decimator_in
mkfifo _data_control/bw_decimator_in
mkfifo _data_control/fw_decimator_out
mkfifo _data_control/bw_decimator_out

# Delay Synchronizer
rm _data_control/fw_delay_sync_iq  2> /dev/null
rm _data_control/bw_delay_sync_iq  2> /dev/null
rm _data_control/fw_delay_sync_hwc 2> /dev/null
rm _data_control/bw_delay_sync_hwc 2> /dev/null
mkfifo _data_control/fw_delay_sync_iq
mkfifo _data_control/bw_delay_sync_iq
mkfifo _data_control/fw_delay_sync_hwc
mkfifo _data_control/bw_delay_sync_hwc

# IQ Saver — FIFO de contrôle start/stop
rm _data_control/iq_saver_control 2> /dev/null
mkfifo _data_control/iq_saver_control
echo -e "\e[92mControl FIFOs created [ OK ]\e[39m"

# ── Suppression des anciens logs ──────────────────────────────────────────────
rm _logs/*.log 2> /dev/null

# ── Configuration USB (libusb buffer illimité) ────────────────────────────────
sudo sh -c "echo 0 > /sys/module/usbcore/parameters/usbfs_memory_mb"

# ── Nettoyage du cache page ───────────────────────────────────────────────────
echo '3' | sudo tee /proc/sys/vm/drop_caches > /dev/null

# ── Génération des coefficients FIR ──────────────────────────────────────────
python3 fir_filter_designer.py
out=$?
if test $out -ne 0; then
    echo -e "\e[91mFIR filter design failed — DAQ chain not started!\e[39m"
    exit 1
fi

# ─────────────────────────────────────────────────────────────────────────────
#  Démarrage de la chaîne DAQ
# ─────────────────────────────────────────────────────────────────────────────
echo -e "\e[92mStarting DAQ Subsystem (KrakenSDR — 5 channels)\e[39m"

# Thread 0 : Realtek DAQ → Rebuffer (pipeline stdio)
chrt -f 99 _daq_core/rtl_daq.out 2> _logs/rtl_daq.log | \
chrt -f 99 _daq_core/rebuffer.out 0 2> _logs/rebuffer.log &

# Thread 1 : Decimateur FIR
chrt -f 99 _daq_core/decimate.out 2> _logs/decimator.log &

# Thread 2 : Delay Synchronizer
chrt -f 99 python3 _daq_core/delay_sync.py 2> _logs/delay_sync.log &

# Thread 3 : Hardware Controller (requiert sudo pour I2C)
chrt -f 99 sudo env "PATH=$PATH" python3 _daq_core/hw_controller.py 2> _logs/hwc.log &

# Thread 4 : IQ Saver — écriture CF32 brut sur disque (remplace iq_server)
#
#   Modes supportés dans daq_chain_config.ini → [data_interface] out_data_iface_type :
#     dat   → démarre iq_saver (recommandé)
#     shmem → démarre aussi iq_saver (données déjà en mémoire partagée)
#
if [ "$out_data_iface_type" = "dat" ] || [ "$out_data_iface_type" = "shmem" ]; then
    echo -e "\e[92mOutput : IQ disk saver (CF32 → _daq_raw/)\e[39m"
    chrt -f 99 _daq_core/iq_saver.out 2> _logs/iq_saver.log &
    IQ_SAVER_PID=$!
    echo "iq_saver PID = $IQ_SAVER_PID"
    echo $IQ_SAVER_PID > _data_control/iq_saver.pid
elif [ "$out_data_iface_type" = "eth" ]; then
    echo -e "\e[93mWARN: out_data_iface_type=eth mais iq_server a été supprimé.\e[39m"
    echo -e "\e[93m      Changez en 'dat' dans daq_chain_config.ini\e[39m"
else
    echo -e "\e[91mUnknown out_data_iface_type='$out_data_iface_type' — iq_saver not started\e[39m"
fi

# ── Affichage des infos de contrôle ──────────────────────────────────────────
echo ""
echo -e "  \e[96m┌─────────────────────────────────────────────────┐\e[39m"
echo -e "  \e[96m│  IQ Saver — commandes de contrôle              │\e[39m"
echo -e "  \e[96m│                                                 │\e[39m"
echo -e "  \e[96m│  Démarrer  : echo START > _data_control/iq_saver_control\e[39m"
echo -e "  \e[96m│  Arrêter   : echo STOP  > _data_control/iq_saver_control\e[39m"
echo -e "  \e[96m│  Terminer  : echo EXIT  > _data_control/iq_saver_control\e[39m"
echo -e "  \e[96m│  Logs      : tail -f _logs/iq_saver.log        │\e[39m"
echo -e "  \e[96m│  Fichiers  : ls -lh _daq_raw/                  │\e[39m"
echo -e "  \e[96m└─────────────────────────────────────────────────┘\e[39m"
echo ""
echo -e "  \e[93mMATLAB (adapter num_ch=5 et cpi_size=1048576) :\e[39m"
echo -e "  \e[93m  fid = fopen('_daq_raw/iq_YYYYMMDD_HHMMSS.dat','rb');\e[39m"
echo -e "  \e[93m  raw = fread(fid,[2*5,1048576],'float32');            \e[39m"
echo -e "  \e[93m  iq  = complex(raw(1:2:end,:),raw(2:2:end,:));        \e[39m"
echo -e "  \e[93m  fclose(fid);                                         \e[39m"
echo ""
echo -e "      )  (     "
echo -e "      (   ) )  "
echo -e "       ) ( (   "
echo -e "     _______)_ "
echo -e "  .-'---------|"
echo -e " (  |/\/\/\/\/|"
echo -e "  '-./\/\/\/\/|"
echo -e "    '_________'"
echo -e "     '-------' "
echo -e "               "
echo -e "Have a coffee, watch radar — enregistrement en cours dans _daq_raw/"
