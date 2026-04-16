# Heimdall DAQ - Custom Firmware for KrakenSDR & KerberosSDR
This is a customized, performance-oriented version of the original Heimdall DAQ Firmware for Coherent Multi-channel SDRs (KerberosSDR & KrakenSDR). It has been specifically tailored for real-time DSP applications, adaptive beamforming, and reliable data acquisition.

## Why a Customized Firmware?
This modification was born out of necessity during my end-of-study project focused on Direction of Arrival (DoA) estimation and Adaptive Beamforming. To validate my algorithms with real-world RF signals, the KrakenSDR was the perfect hardware choice. However, extracting raw, synchronized I/Q data reliably proved to be a major bottleneck.

The Problem with the Original Firmware:
The stock Heimdall firmware relies on a rigid TCP server for IQ data streaming. When attempting to stream high-bandwidth IQ data to GNU Radio or MATLAB (especially over a Wi-Fi link from a Raspberry Pi 4), the network latency caused the TCP buffers to fill up quickly. This resulted in "TCP Window Stalls", dropped packets, and ultimately, frozen sockets that completely crashed the GNU Radio flowgraphs. Even the pre-built aarch64 images suffered from these network-induced crashes during live testing.

The Solution:
After several nights of debugging, I decided to bypass the faulty TCP pipeline and re-architect the data export layer. This custom repository introduces two robust solutions to extract IQ data without breaking the hardware phase-synchronization:

A Fire-and-Forget UDP Server: Replaced the blocking TCP handshake with a lightweight, fragmented UDP streamer.

A Standalone Offline Logger: A dedicated module to dump the synchronized shared memory directly to the local storage for post-processing.


HAMMADI LYES
