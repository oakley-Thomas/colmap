#docker pull colmap/colmap:latest
docker run --gpus all -w /working -v $1:/working -it image-preprocess:latest
