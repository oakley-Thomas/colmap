#docker build -t="colmap:latest" .
# In some cases, you may have to explicitly specify the compute architecture:
docker build -t="image-preprocess:latest" --build-arg CUDA_ARCHITECTURES=75 .
