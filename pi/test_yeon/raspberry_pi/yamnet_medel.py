#Semantic inference 담당
import tensorflow_hub as hub

yamnet = hub.load(
    "https://tfhub.dev/google/yamnet/1"
)

def run_yamnet(audio_window):
    """0.96초 audio
    출력:
    scores -> 의미: semantic score -> 현재 이게 중심 
    embeddings -> 의미: feature vector
    spectrogram -> 의미: mel spectrogram
    -> 혹시나 싶어서 전에 했던 것도 적어두었습니다. 나중에 개념이 필요하면 여기 보세요
    """ 

    scores, embeddings, spectrogram = yamnet(
        audio_window
    )

    return (
        scores.numpy(),
        embeddings.numpy(),
        spectrogram.numpy()
    )