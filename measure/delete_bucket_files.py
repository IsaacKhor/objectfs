import boto3
import sys, os
from boto3.s3.transfer import TransferConfig

url = 'http://' + os.environ['S3_HOSTNAME']
bucket_name = sys.argv[1]
access_key = os.environ['S3_ACCESS_KEY_ID']
secret_key = os.environ['S3_SECRET_ACCESS_KEY']

s3 = boto3.resource('s3', endpoint_url=url, use_ssl=False, 
        aws_access_key_id=access_key, aws_secret_access_key=secret_key)

GB = 1024 ** 3
config = TransferConfig(multipart_threshold=5 * GB, max_concurrency=10, use_threads=True)

print(f'Deleting everything in bucket {bucket_name}')

bucket = s3.Bucket(bucket_name)
for bucket_object in bucket.objects.all():
    bucket_object.delete()

bucket = s3.Bucket(bucket_name)
for my_bucket_object in bucket.objects.all():
    print(my_bucket_object)
