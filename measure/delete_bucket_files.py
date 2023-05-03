import boto3
import sys
from boto3.s3.transfer import TransferConfig

url = sys.environ['S3_HOSTNAME']
bucket = sys.argv[2]
access_key = sys.environ['S3_ACCESS_KEY']
secret_key = sys.environ['S3_SECRET_ACCESS_KEY']

s3 = boto3.resource('s3', endpoint_url=url, use_ssl=False, 
        aws_access_key_id=access_key, aws_secret_access_key=secret_key)

GB = 1024 ** 3
config = TransferConfig(multipart_threshold=5 * GB, max_concurrency=10, use_threads=True)

bucket = s3.Bucket(bucket)
for bucket_object in bucket.objects.all():
    if bucket_object.key != 'prefix.00000000':
        bucket_object.delete()
    

bucket = s3.Bucket(bucket)
for my_bucket_object in bucket.objects.all():
    print(my_bucket_object)
