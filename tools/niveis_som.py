"""Mostra o volume (RMS) de um som cru gravado com AUDIO_WAV (estéreo 16 bits, 32728 Hz), por segundo.
   python3 tools/niveis_som.py som.raw [saida.wav]   (com saida.wav, também converte para WAV)"""
import array, math, sys, wave

RATE = 32728
dados = array.array('h', open(sys.argv[1], 'rb').read())
seg = RATE * 2
for i in range(0, len(dados), seg):
    b = dados[i:i + seg]
    if not b:
        break
    rms = math.sqrt(sum(x * x for x in b) / len(b))
    print(f'{i // seg:4d}s  rms {rms:8.1f}  ' + '#' * min(60, int(rms / 100)))
if len(sys.argv) > 2:
    with wave.open(sys.argv[2], 'wb') as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(dados.tobytes())
