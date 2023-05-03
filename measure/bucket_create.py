import boto3
from boto3.s3.transfer import TransferConfig
url = 'http://10.1.0.6:7480'
access_key = 'FK31TLLZXJ0UHLG9PR0K'
secret_key = 'KGuwIt2AnNay3tsYLWz2iV7STRCA7Kbr8MV5ex0I'

s3 = boto3.resource('s3', endpoint_url=url, use_ssl=False, 
        aws_access_key_id=access_key, aws_secret_access_key=secret_key)

GB = 1024 ** 3
config = TransferConfig(multipart_threshold=5 * GB, max_concurrency=10, use_threads=True)

'''
for bucket in s3.buckets.all():
    print(bucket.name)
    for my_bucket_object in bucket.objects.all():
        print(my_bucket_object)
        '''

bucket = s3.Bucket('fsbench')
bucket.objects.all().delete()
bucket.delete()

s3.create_bucket(Bucket='fsbench')
bucket = s3.Bucket('fsbench')

file_path = "../code/prefix.00000000"
bucket.upload_file(file_path, 'prefix.00000000')

for my_bucket_object in bucket.objects.all():
    print(my_bucket_object)

#s3.Object('bucket1', 'prefix1.00000000').delete()