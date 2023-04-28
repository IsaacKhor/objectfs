import boto3
import os
from boto3.s3.transfer import TransferConfig
url = 'http://localhost:9000'
access_key = 'minioadmin'
secret_key = 'minioadmin'

s3 = boto3.resource('s3', endpoint_url=url, use_ssl=False, 
        aws_access_key_id=access_key, aws_secret_access_key=secret_key)

GB = 1024 ** 3
config = TransferConfig(multipart_threshold=5 * GB, max_concurrency=10, use_threads=True)

bucket = s3.Bucket('filebench')
for bucket_object in bucket.objects.all():
    if bucket_object.key != 'prefix.00000000':
        bucket_object.delete()
    

bucket = s3.Bucket('filebench')
for my_bucket_object in bucket.objects.all():
    print(my_bucket_object)
