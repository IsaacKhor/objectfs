sudo filebench -f ./fileserver.f
python3 ../code/minio_cli.py

sudo filebench -f ./oltp.f
python3 ../code/minio_cli.py

sudo filebench -f ./random_write.f
python3 ../code/minio_cli.py

sudo filebench -f ./webserver.f
python3 ../code/minio_cli.py
